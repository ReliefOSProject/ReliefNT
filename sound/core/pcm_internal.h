#ifndef RELIEFNT_SOUND_CORE_PCM_C_INTERNAL_H
#define RELIEFNT_SOUND_CORE_PCM_C_INTERNAL_H

#include <limits.h>
#include <sound/asound.h>
#include <stddef.h>

#include <linux/errno.h>
#include <linux/time.h>
#include <reliefnt/time.h>
#include <reliefnt/audio.h>
#include <reliefnt/lock.h>
#include <reliefnt/heap.h>

#define AUDIO_PCM_MAX 32u
#define AUDIO_PCM_BUFFER_BYTES 524288u
#define AUDIO_PCM_BOUNDARY_BASE (1ULL << 60)
#define AUDIO_PCM_MMAP_PAGE_BYTES 4096u
#define AUDIO_PCM_PROT_READ 0x1u
#define AUDIO_PCM_PROT_WRITE 0x2u

struct audio_pcm {
    struct module *code_owner;
    uint32_t live;
    uint32_t card;
    uint32_t stream;
    uint32_t device;
    enum audio_direction direction;
    enum audio_pcm_state state;
    struct audio_caps caps;
    struct audio_params params;
    struct audio_params_ext selected;
    struct audio_format_caps format_caps;
    uint32_t explicit_format;
    struct audio_dma dma;
    uint8_t *extended_buffer;
    void *extended_storage;
    uint64_t hw_ptr;
    uint64_t driver_frames;
    uint64_t appl_ptr;
    uint64_t boundary;
    uint64_t wake_seq;
    uint64_t avail_min;
    uint32_t auto_start;
    uint64_t start_threshold;
    uint64_t stop_threshold;
    uint32_t tstamp_type;
    uint32_t tstamp_mode;
    struct linux_timespec trigger_tstamp;
    uint64_t silence_threshold;
    uint64_t silence_size;
    uint64_t silence_origin;
    uint64_t silence_covered;
    uint32_t silence_initialized;
    uint32_t generation;
    uint32_t mapping_refs;
    uint32_t data_refs;
    uint32_t control_refs;
    void *metadata_storage;
    uint8_t *status_page;
    uint8_t *control_page;
    uint32_t file_refs;
    uint32_t close_pending;
    int error;
    struct audio_pcm *link;
    struct kernel_spinlock lock;
};




extern struct audio_pcm pcm_slots[AUDIO_PCM_MAX];

/** @brief Borrow the allocated PCM ring's CPU alias.
 * @param pcm Stable caller-owned PCM, serialized against reconfiguration.
 * @return Borrowed pointer or NULL before HW_PARAMS; no locks or ownership change.
 */
uint8_t *audio_pcm_buffer(struct audio_pcm *pcm);

/** @brief Free data and metadata backing after all file/VMA/STOP leases retire.
 * @param pcm Exclusively cleanup-owned PCM; allocations and provider pin cleared.
 * Task context without the PCM or module registry lock; consumes owned memory.
 */
void audio_pcm_free_buffer(struct audio_pcm *pcm);

/** @brief Prepare aligned page-backed data without publishing partial ownership.
 * @param pcm Stable caller-owned PCM with serialized reconfiguration.
 * @param bytes Validated nonzero ring bytes, bounded by AUDIO_PCM_BUFFER_BYTES.
 * @param new_storage Owned allocation output, NULL when existing backing reused.
 * @param new_buffer Borrowed aligned CPU alias for that owner.
 * @return Zero or EINVAL/ENOMEM. Task context without locks; caller commits or
 * frees new_storage on every later failure, leaving existing mappings intact.
 */
int audio_pcm_prepare_buffer(struct audio_pcm *pcm, uint32_t bytes,
                                    void **new_storage, uint8_t **new_buffer);

/** @brief Silence uncommitted playback samples without committing user frames.
 * @param pcm Configured PCM under its IRQ-safe lock; task or IRQ context.
 * @return None. Tracks silence independently of appl_ptr and never moves it.
 */
void audio_pcm_silence_locked(struct audio_pcm *pcm);

/** @brief Publish only trusted state/counters to the user-readable metadata page.
 * @param pcm Live core object under its IRQ-safe lock; the page is core-owned.
 * @return None. Task/IRQ safe; release stores order DMA-visible progress.
 */
void audio_pcm_publish_locked(struct audio_pcm *pcm);

/** @brief Publish committed control values after a task-context pointer change.
 * @param pcm Core object under its IRQ-safe lock; no ownership transfer.
 * @return None. Does not run for ordinary IRQ progress, preserving user updates.
 */
void audio_pcm_publish_control_locked(struct audio_pcm *pcm);

/** @brief Validate the ALSA boundary before importing shared control values.
 * @param pcm Configured live object under its IRQ-safe lock.
 * @return Zero or -EINVAL; no core field changes for an invalid pointer.
 * Task/IRQ context; bounded acquire loads pair with client release stores.
 */
int audio_pcm_import_control_locked(struct audio_pcm *pcm);

/** @brief Import forward progress without letting a delayed IRQ rewind HWSYNC.
 * @param pcm Live configured PCM under its IRQ-safe lock.
 * @param frames Cumulative driver frames in the current prepare epoch.
 * @return None. Compare cumulative frames before wrapping the public pointer.
 */
void audio_pcm_progress_locked(struct audio_pcm *pcm, uint64_t frames);

/** @brief Update permanent PCM metadata after a pinned registry notification.
 * @param stream Core generation stream token; stale/disconnected slots are ignored.
 * @param frames Cumulative completed driver frames in the prepare epoch.
 * @param error Zero, negative XRUN error, or -ENODEV on forced disconnect.
 * @return None. IRQ/task safe, bounded by AUDIO_PCM_MAX, no callbacks or sleep.
 */
void audio_pcm_notify(uint32_t stream, uint64_t frames, int error);

void audio_pcm_reset_locked(struct audio_pcm *pcm);

/** @brief Test whether an operation may commit to this PCM generation.
 * @param pcm Borrowed PCM under its IRQ-safe lock, in task or IRQ context.
 * @return True while live and connected; forced disconnect is irreversible.
 */
bool audio_pcm_connected_locked(const struct audio_pcm *pcm);

/** @brief Consume coalesced progress without changing disconnected metadata.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @return Event count, XRUN/input errno, or ENODEV after permanent disconnect.
 * No PCM lock spans registry polling or a pinned hardware STOP callback.
 */
int audio_pcm_event(struct audio_pcm *pcm);

/** @brief Snapshot state for an operation requiring a connected generation.
 * @param pcm Borrowed non-NULL PCM, serialized in task context.
 * @param state Caller-owned writable state output, unchanged on failure.
 * @return Zero or ENODEV; an IRQ-safe lock protects the snapshot.
 */
int audio_pcm_live_state(struct audio_pcm *pcm, enum audio_pcm_state *state);

struct audio_pcm *audio_pcm_open(uint32_t card, uint32_t device,
                                 enum audio_direction direction, int *error);

/** @brief Copy the live PCM capability snapshot without exposing PCM storage.
 * @param pcm Live PCM object.
 * @param caps Writable capability output.
 * @return Zero on success, or -ENODEV/-EINVAL.
 */
int audio_pcm_caps(struct audio_pcm *pcm, struct audio_caps *caps);

/** @brief Validate and retain one PCM data/status/control mapping. */
int audio_pcm_mmap_acquire(struct audio_pcm *pcm, uint64_t offset,
                           uint64_t length, uint32_t prot,
                           struct audio_pcm_mmap *mapping);

/** @brief Retain the backing owned by an existing VMA, without reconfiguring it.
 * @param pcm Borrowed PCM whose existing mapping keeps its backing live.
 * @param region Region recorded in that VMA; unchanged by protection or splits.
 * @return Zero or -EINVAL for an invalid reference. Task context, IRQ-safe lock.
 * The new reference must be released on child exit or removal of the split VMA.
 */
int audio_pcm_mmap_retain(struct audio_pcm *pcm, enum audio_pcm_mmap_region region);

/** @brief Release one PCM mapping reference retained by audio_pcm_mmap_acquire. */
void audio_pcm_mmap_release(struct audio_pcm *pcm, enum audio_pcm_mmap_region region);

/** @brief Select the legacy signed sample format by its precision.
 * @param pcm Borrowed OFD-owned PCM, serialized in task context.
 * @param params Borrowed hardware geometry and precision.
 * @return Zero or state/geometry errno. No ownership transfer; IRQ-safe commit.
 */
int audio_pcm_hw_params(struct audio_pcm *pcm, const struct audio_params *params);

/** @brief Query the actual selected-format and DMA geometry constraints.
 * @param pcm Borrowed live OFD-owned PCM, serialized in task context.
 * @param caps Receives a copy; caller owns the output storage.
 * @return Zero or -EINVAL/-ENODEV; IRQ-safe snapshot, no module callback.
 */
int audio_pcm_format_caps(struct audio_pcm *pcm, struct audio_format_caps *caps);

/** @brief Commit an explicit ALSA/OSS memory format and subformat.
 * @param pcm Borrowed OFD-owned PCM, serialized in task context.
 * @param selection Borrowed exact format, precision and DMA geometry.
 * @return Zero or -EINVAL/state/device errno. No ownership transfer.
 * Hardware preparation occurs later; IRQ-safe commit never invokes the driver.
 */
int audio_pcm_hw_params_format(struct audio_pcm *pcm,
                                const struct audio_params_ext *selection);

/** @brief Release a stopped PCM configuration while retaining its open lease.
 * @param pcm OFD-owned stream, serialized in task context; must be non-NULL.
 * @return Zero, -EBADFD unless SETUP/PREPARED without data mappings, or errno.
 * Hardware STOP precedes clearing the core ring; no backing page is freed.
 * A concurrent fatal disconnect returns -ENODEV even if STOP reports an error.
 */
int audio_pcm_hw_free(struct audio_pcm *pcm);

/** @brief Return the Linux identity of the OFD-owned PCM substream.
 * @param pcm Live core object; borrowed and serialized by task execution.
 * @param info Writable kernel-owned ALSA identity output.
 * @return Zero or -EINVAL/-ENODEV. No ownership transfer or user dereference.
 */
int audio_pcm_info(struct audio_pcm *pcm, struct snd_pcm_info *info);

/** @brief Commit validated Linux software thresholds and clock selection.
 * @param pcm Configured live object borrowed by the OFD in task context.
 * @param params In/out kernel copy; boundary is returned on success.
 * @return Zero or -EINVAL/-EBADFD/-ENODEV. Validate before mutation under
 * the IRQ-safe PCM lock; caller retains all storage and reference ownership.
 */
int audio_pcm_sw_params(struct audio_pcm *pcm, struct snd_pcm_sw_params *params);

/** @brief Select the Linux clock used by status and mmap timestamps.
 * @param pcm Borrowed live core object in serialized task context.
 * @param type Realtime, monotonic, or monotonic-raw ALSA clock selector.
 * @return Zero or -EINVAL/-ENODEV; selection changes under the IRQ-safe lock.
 */
int audio_pcm_tstamp(struct audio_pcm *pcm, int type);

/** @brief Report the selected interleaved mmap channel layout.
 * @param pcm Configured OFD-owned object; borrowed in task context.
 * @param info In/out kernel copy containing a zero-based channel index.
 * @return Zero, -EINVAL for invalid channel, or -EBADFD before HW_PARAMS.
 */
int audio_pcm_channel_info(struct audio_pcm *pcm, struct snd_pcm_channel_info *info);

/** @brief Refresh progress and enforce Linux HWSYNC state errors.
 * @param pcm Borrowed live object; no reference transfer in task context.
 * @return Zero, -EPIPE for XRUN, -EBADFD for OPEN/SETUP, or device errno.
 * Running/draining streams query the pinned driver outside the PCM lock, then
 * recheck connection/state before publishing; no timer periods are invented.
 */
int audio_pcm_hwsync(struct audio_pcm *pcm);

/** @brief Move the application pointer by bounded committed/available frames.
 * @param pcm Borrowed configured object serialized in task context.
 * @param frames Requested frame count; zero is a successful no-op in any state.
 * @param forward Nonzero to advance, zero to rewind.
 * @return Actual moved frames or negative Linux state/device errno. Updates
 * control metadata under the IRQ-safe lock and never accesses user memory.
 */
long audio_pcm_move(struct audio_pcm *pcm, uint64_t frames, int forward);

/** @brief Prepare configured DMA without reviving a disconnected generation.
 * @param pcm Borrowed configured PCM, serialized in task context.
 * @return Zero or state/hardware errno; a concurrent disconnect returns ENODEV even on callback error.
 * XRUN recovery first proves STOP; failure retains state, pointers and DMA. Old
 * stopped-run notifications are discarded before the new pointer epoch. No PCM
 * lock spans a pinned module callback; commit rechecks under the lock.
 */
int audio_pcm_prepare(struct audio_pcm *pcm);

/** @brief Start a PCM and its linked peer without reviving disconnected state.
 * @param pcm Borrowed master PCM, serialized in task context.
 * @return Zero or start/STOP/state errno; disconnected rollback returns ENODEV.
 * Failed rollback STOP retains DMA and publishes XRUN rather than PREPARED.
 */
int audio_pcm_start(struct audio_pcm *pcm);

/** @brief Stop and reset a connected PCM while preserving fatal metadata.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @return Zero or state/hardware errno; disconnect at commit overrides callback errors with -ENODEV.
 */
int audio_pcm_drop(struct audio_pcm *pcm);

/** @brief Reset stream pointers and backing samples while preserving state. */
int audio_pcm_reset(struct audio_pcm *pcm);

/** @brief Stop a connected stream and publish an XRUN condition.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @return STOP result or -ENODEV on disconnect, overriding other callback errors.
 * Disconnected metadata remains immutable.
 */
int audio_pcm_xrun(struct audio_pcm *pcm);

/** @brief Read or commit Linux SYNC_PTR fields atomically.
 * @param pcm Live OFD-owned stream in serialized task context.
 * @param flags APPL/AVAIL_MIN set request reads; clear request commits. HWSYNC polls.
 * @param appl_ptr Proposed pointer only when APPL is clear, below boundary.
 * @param avail_min Proposed wake threshold only when AVAIL_MIN is clear.
 * @param status Caller-owned writable snapshot, required.
 * @return Zero, -EINVAL for invalid flags/pointer, or device errno. No ownership
 * transfer; all validation precedes mutations under the IRQ-safe PCM lock.
 */
int audio_pcm_sync(struct audio_pcm *pcm, uint32_t flags,
                   uint64_t appl_ptr, uint64_t avail_min,
                   struct audio_pcm_status *status);

/** @brief Pause or resume without overwriting a concurrent forced disconnect.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @param pause True for PAUSE, false for UNPAUSE.
 * @return Zero or state/hardware errno; disconnect at commit overrides callback errors with -ENODEV.
 */
int audio_pcm_pause(struct audio_pcm *pcm, bool pause);

/** @brief Drain submitted playback frames, accepting the final period's overshoot.
 * @param pcm Live configured PCM; capture delegates to DROP.
 * @return 0 after safe STOP, -EAGAIN while queued, or state/hardware errno.
 * Task context; no PCM lock is held across driver START/STOP callbacks. DMA
 * tail padding does not advance appl_ptr; failed STOP keeps DRAINING retryable.
 * Fatal disconnect overrides callback errors with -ENODEV.
 */
int audio_pcm_drain(struct audio_pcm *pcm);

int audio_pcm_link(struct audio_pcm *master, struct audio_pcm *slave);

int audio_pcm_unlink(struct audio_pcm *pcm);

void audio_pcm_copy_in(struct audio_pcm *pcm, uint64_t offset,
                              const uint8_t *source, uint32_t bytes);

void audio_pcm_copy_out(const struct audio_pcm *pcm, uint64_t offset,
                               uint8_t *target, uint32_t bytes);

/** @brief Transfer complete frames through the configured interleaved DMA ring.
 * @param pcm Borrowed live PCM, serialized in task context; must be non-NULL.
 * @param kernel_data Kernel-owned sample storage; writable for capture, readable
 * for playback, with at least frames times frame_bytes bytes; NULL only if zero.
 * @param frames Requested number of complete frames.
 * @return Frames copied (including short progress), or negative state/driver
 * errno when no progress is made. Auto-start failure after playback progress is
 * latched for the next call; permanent disconnect metadata is never overwritten.
 * No PCM lock spans a driver callback. The caller retains sample ownership.
 */
long audio_pcm_transfer(struct audio_pcm *pcm, void *kernel_data, uint32_t frames);

int audio_pcm_status(struct audio_pcm *pcm, struct audio_pcm_status *status);

/** @brief Drop the final file reference while retaining DMA on failed STOP.
 * @param pcm Borrowed live PCM owned by the closing OFD; may be NULL.
 * @return None. Task context. Failed hardware close retains the core lease and
 * backing even after the last munmap; manager task service retries retirement.
 * A fatal DISCONNECTED state and its ENODEV error survive failed close callbacks.
 */
void audio_pcm_release(struct audio_pcm *pcm);

/** @brief Inspect whether final OFD releases retained an unsafe hardware lease.
 * @return True while task retirement is needed. IRQ safe, no module callback.
 */
bool audio_pcm_release_work_pending(void);

/** @brief Retry retained final releases with module images protected by manager admission.
 * @param budget Maximum PCM close attempts in this task phase.
 * @return Attempt count. Task context without execution ownership. A successful
 * close marks retirement ready; backing remains reserved until the manager
 * reacquires execution ownership and publishes retirement.
 */
uint32_t audio_pcm_retry_releases(uint32_t budget);

/** @brief Publish completed retirements after manager execution ownership resumes.
 * @return None. Task context under execution ownership. Serializing with opens
 * prevents a new PCM from reinitializing a slot while its old lock is held.
 * VMA releases keep the slot reserved while close_pending is nonzero.
 */
void audio_pcm_finish_releases(void);

#endif

#ifndef RELIEFNT_AUDIO_H
#define RELIEFNT_AUDIO_H
#include <reliefos/driver_audio.h>
#include <reliefnt/types.h>
struct task;
/** @brief Copy validated card metadata and bind its module owner.
 * @param identity Borrowed identity copied on success.
 * @param ops Borrowed operation table copied on success.
 * @param opaque Module context, alive until unregister returns.
 * @param owner Driver-manager slot owning callbacks.
 * @param out_id Receives generation token; untouched on failure.
 * @return 0, -EINVAL, or -ENOSPC. Task context; short IRQ-safe lock only.
 */
int audio_register_card_owned(const struct audio_card_identity *identity,
                             const struct audio_card_ops *ops, void *opaque,
                             uint32_t owner, uint32_t *out_id);
/** @brief Set the card service, synchronizing replaced callbacks.
 * @param id Generation card token.
 * @param callback Nonblocking service, or NULL to remove.
 * @param opaque Borrowed context held alive until replacement/removal returns.
 * @return 0, -ENODEV or -EBUSY. Task context, no execution lock/callback nesting.
 */
int audio_card_set_service(uint32_t id, audio_service_fn callback, void *opaque);
/** @brief Run one globally bounded tick, rotating the starting card for fairness.
 * @return None. IRQ context; each callback is pinned and invoked without lock.
 * Callbacks consume at most a total of 32 completions and must never sleep.
 * Optional task callbacks are coalesced for a later task-context pump.
 */
void audio_service_tick(void);
/** @brief Inspect coalesced work for live cards with task callbacks.
 * @return True when a card needs task-context service. IRQ safe; no callbacks.
 */
bool audio_task_work_pending(void);
/** @brief Run bounded, pinned task callbacks, visiting each card at most once.
 * @param budget Global work limit; zero preserves all pending work.
 * @return Work consumed, no greater than budget. Caller owns manager lifecycle
 * admission and has released execution ownership with IRQs enabled. No core
 * lock spans a callback; IRQ service can progress while task callbacks wait.
 */
uint32_t audio_process_task_work(uint32_t budget);
/** @brief Queue fatal disconnect for cards sharing a module owner and PCI BDF.
 * @param card Live generation token identifying the affected controller.
 * @return Zero or -ENODEV. IRQ safe; closes admission and wakes readers with
 * ENODEV without any module callback, DMA stop, wait or resource release.
 */
int audio_request_controller_disconnect(uint32_t card);
/** @brief Inspect whether fatal disconnect requests need task-context service.
 * @return True for pending cards, including failed STOP retries. IRQ safe.
 */
bool audio_disconnect_work_pending(void);
/** @brief Stop and disconnect a bounded number of pending cards in task context.
 * @param budget Maximum card attempts, including failed STOP attempts.
 * @return Attempt count. Caller owns manager lifecycle admission and has
 * released execution ownership; module images remain mapped throughout.
 * Failed STOP keeps DMA, card metadata and the pending request for retry.
 */
uint32_t audio_process_pending_disconnects(uint32_t budget);
/** @brief Open a core-owned stream lease and serialize its eventual disconnect.
 * @param card Generation token for card.
 * @param device PCM device index.
 * @param direction Valid playback/capture selector.
 * @param out_id Receives stream generation token.
 * @return 0 or negative errno. Task context without execution lock; callback may wait.
 */
int audio_stream_open(uint32_t card, uint32_t device, enum audio_direction direction, uint32_t *out_id);
/** @brief Query the basic PCM capabilities of a card through its pinned callback. */
int audio_card_pcm_caps(uint32_t card, uint32_t device, enum audio_direction direction,
                        struct audio_caps *caps);
/** @brief Copy live card identity. */
int audio_card_identity(uint32_t card, struct audio_card_identity *identity);
struct snd_pcm_info;
/** @brief Report one real PCM device using Linux INFO layout.
 * @param card Live generation token; registry ordinal is returned in INFO.
 * @param info In/out kernel-accessible storage with device/subdevice/stream selectors.
 * @return Zero, -ENXIO for absent device/subdevice/direction, or device errno.
 * Task context; module callbacks run pinned without the registry lock. No
 * reference ownership changes; subdevices_avail reflects active core leases.
 */
int audio_card_pcm_info(uint32_t card, struct snd_pcm_info *info);
/** @brief Return fixed real-control count for a live card. */
int audio_card_control_count(uint32_t card, uint32_t *count);
/** @brief Query one real control's metadata. */
int audio_card_control_info(uint32_t card, uint32_t control,
                            struct audio_control_info *info);
/** @brief Read one real control value. */
int audio_card_control_read(uint32_t card, uint32_t control,
                            struct audio_control_value *value);
/** @brief Write one real control value. */
int audio_card_control_write(uint32_t card, uint32_t control,
                             const struct audio_control_value *value);
#define AUDIO_CONTROL_EVENT_SLOTS 64U
/* One pending mask per fixed card element; order retains first-notification
 * FIFO semantics. All fields are protected by the audio registry lock.
 * Storage belongs to the OFD and remains alive until queue_close completes. */
struct audio_control_event_queue {
    uint32_t card, subscribed, head, length;
    uint32_t pending[AUDIO_CONTROL_EVENT_SLOTS];
    uint32_t order[AUDIO_CONTROL_EVENT_SLOTS];
};
/** @brief Attach preallocated OFD event storage to a live fixed-control card.
 * @param card Live generation token; no module reference is transferred.
 * @param queue Unregistered writable storage retained until queue_close.
 * @return Zero, -EINVAL, -ENODEV, -EBUSY, or -ENFILE; no callback or allocation.
 */
int audio_control_queue_open(uint32_t card, struct audio_control_event_queue *queue);
/** @brief Detach event storage before its OFD is released or reused.
 * @param queue Registered storage; remains valid throughout this call.
 * @return Zero or -EBADF; IRQ-safe serialization, also after card disconnect.
 */
int audio_control_queue_close(struct audio_control_event_queue *queue);
/** @brief Change subscription, clearing pending masks on disable or first enable.
 * @param queue Registered OFD storage, serialized against notifications.
 * @param enable Nonzero enables; repeated enable preserves pending events.
 * @return Zero, -EBADF, or -ENODEV; no callback, allocation, or sleep.
 */
int audio_control_queue_subscribe(struct audio_control_event_queue *queue, int enable);
/** @brief Atomically inspect or dequeue the oldest merged element notification.
 * @param queue Registered OFD storage.
 * @param control Receives zero-based element index; must be writable.
 * @param mask Receives merged Linux event bits; must be writable.
 * @param consume Nonzero removes this item; later notifications remain pending.
 * @return Zero, -EINVAL, -EBADF, -EBADFD, -ENODEV, or -EAGAIN; IRQ safe.
 */
int audio_control_queue_next(struct audio_control_event_queue *queue,
                             uint32_t *control, uint32_t *mask, int consume);
/** @brief Dispatch prepare through a pinned module callback.
 * @param id Core stream token.
 * @param params Borrowed validated hardware parameters.
 * @param dma Borrowed core-owned pinned buffer; not freed by this function.
 * @return Driver status or -ENODEV/-EINVAL. Task context without execution lock.
 */
int audio_stream_prepare(uint32_t id, const struct audio_params *params, const struct audio_dma *dma);
/** @brief Query optional selected-format capabilities through a pinned card callback.
 * @param card Generation token for a live card.
 * @param device PCM device index.
 * @param direction Playback/capture selector.
 * @param caps Receives bounded memory-format and DMA geometry metadata.
 * @return 0, -ENODEV, -EOPNOTSUPP, or callback errno. Task context only.
 */
int audio_stream_format_caps(uint32_t card, uint32_t device,
                             enum audio_direction direction,
                             struct audio_format_caps *caps);
/** @brief Dispatch explicit selected-format prepare through an optional callback.
 * @param id Core stream generation token.
 * @param params Borrowed selected memory format/subformat and significant bits.
 * @param dma Borrowed core-owned pinned DMA; driver does not free it.
 * @return 0, -ENODEV, -EOPNOTSUPP, or callback errno. Task context only.
 */
int audio_stream_prepare_format(uint32_t id, const struct audio_params_ext *params,
                                const struct audio_dma *dma);
/** @brief Trigger an attached hardware stream using a module execution pin.
 * @param id Core stream token.
 * @param trigger START/STOP/PAUSE/UNPAUSE; STOP synchronously quiesces DMA.
 * @return Driver status or -EINVAL/-ENODEV. IRQ safe; callback cannot sleep.
 */
int audio_stream_trigger(uint32_t id, enum audio_trigger trigger);
/** @brief Read monotonic hardware position through a pinned callback.
 * @param id Core stream token.
 * @param frames Receives hardware frames.
 * @return Driver status or -ENODEV/-EINVAL. IRQ safe; never sleeps.
 */
int audio_stream_pointer(uint32_t id, uint64_t *frames);
/** @brief Consume the newest period event for a stream, if one is pending. */
int audio_stream_period_poll(uint32_t id, uint64_t *frames, int *error);
/** @brief Close a stream lease after the last fd/VMA owner drops it.
 * @param id Core-owned stream token. Caller serializes last-reference close.
 * @return 0, queued disconnect errno or STOP error; failure retains the lease
 * and module. Task context
 * without execution lock; detached leases call no module.
 * DMA page release belongs to the PCM owner, after this returns.
 */
int audio_stream_close(uint32_t id);
/** @brief Disconnect a card before its module is finalized.
 * @param id Card generation token.
 * @param force Nonzero permits disconnect of open stream leases.
 * @return 0, -ENODEV, -EBUSY or STOP error (module must remain mapped).
 * Task context, never from callbacks/IRQ. Closing cancels service admission,
 * synchronizes callbacks and stops/closes hardware while preserving fd/VMA leases.
 */
int audio_unregister_card(uint32_t id,uint32_t force);
/** @brief Reserve all a module's cards against new opens before disconnecting.
 * @param owner Driver manager slot; memory must stay alive until return.
 * @param force Nonzero forces hardware disconnect while core leases remain.
 * @return 0, -EBUSY or STOP error; failure forbids module image release.
 * Task context; registry lock provides atomic all-card busy preflight/reservation.
 */
int audio_unregister_owner(uint32_t owner,uint32_t force);
/** @brief Submit a bounded period notification; stale generations are ignored.
 * @param card Card token held by driver.
 * @param stream Driver hardware stream id (matched by later PCM subscriber).
 * @param frames Completed frame count.
 * @param error Negative stream error, or zero.
 * @return None. IRQ safe; only updates core counters, never calls module/userland.
 */
void audio_period_elapsed(uint32_t card, uint32_t stream, uint64_t frames, int error);
/** @brief Fan out one hardware PCM period to selected ALSA timer OFDs.
 * @param card Live card generation token.
 * @param device PCM device index.
 * @param direction Playback or capture direction.
 * @param frames Cumulative hardware frame position.
 * @param error Negative stream error, or zero.
 * @return None. IRQ-safe queueing only; no user memory or module callbacks.
 */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error);
/** @brief Submit a control notification for a live generation.
 * @param card Card token.
 * @param control Hardware control id, retained by later control subscriber.
 * @return None. IRQ safe, no callback or allocation.
 */
void audio_control_changed(uint32_t card, uint32_t control);
/** @brief Record a masked notification and wake registered control readers.
 * @param card Live generation token; stale tokens are ignored.
 * @param control Zero-based real control index.
 * @param mask ALSA event bits to merge into each subscribed OFD's pending item.
 * @return None. IRQ safe; no allocation or module callback.
 */
void audio_control_changed_mask(uint32_t card, uint32_t control,
                                uint32_t mask);
/** @brief Snapshot the control notification/disconnect epoch under the registry lock.
 * @return Wrapping event epoch; compare before probing and after publishing sleep.
 */
uint32_t audio_control_event_epoch(void);
/** @brief Register the current task and publish an interruptible control sleep.
 * @return None. Task context; caller must recheck the epoch after publication.
 * Queue exhaustion leaves the task runnable for the syscall retry fallback.
 */
void audio_control_wait_current(void);
/** @brief Remove a stale or completed control wait registration.
 * @param task Borrowed task pinned by the caller's execution transaction.
 * @return None. Does not change scheduler state; may be called on syscall retry.
 */
void audio_control_wait_remove(struct task *task);
/** @brief Read period count of a live generation for diagnostics.
 * @param card Generation token.
 * @return Count, or zero for stale/disconnected card. IRQ safe, no ownership.
 */
uint64_t audio_card_periods(uint32_t card);
/** @brief Copy the nth live card's token and identity for devfs enumeration.
 * @param index Zero-based ordinal among currently live cards.
 * @param out_id Optional generation token output.
 * @param identity Optional metadata output.
 * @return 0, or -ENOENT when the ordinal is no longer present. Task context.
 */
int audio_card_snapshot(uint32_t index, uint32_t *out_id,
                        struct audio_card_identity *identity);
/** @brief Return the current compact ordinal for a live generation token.
 * @param card Generation token returned by registration.
 * @return Zero-based ordinal, or -ENOENT for a stale/disconnected token.
 */
int audio_card_index(uint32_t card);

enum audio_pcm_state {
    AUDIO_PCM_OPEN = 0,
    AUDIO_PCM_SETUP = 1,
    AUDIO_PCM_PREPARED = 2,
    AUDIO_PCM_RUNNING = 3,
    AUDIO_PCM_XRUN = 4,
    AUDIO_PCM_DRAINING = 5,
    AUDIO_PCM_PAUSED = 6,
    AUDIO_PCM_DISCONNECTED = 8,
};

/* Synthetic /dev/snd node identity.  The card token is generation tagged;
 * keeping it in the node prevents a stale path from binding a new card. */
#define AUDIO_DEVICE_VOLUME_ID(card, device, direction) \
    ((((uint32_t)(card) & 0x00ffffffu) << 8) | \
     (((uint32_t)(device) & 0x7fu) << 1) | ((uint32_t)(direction) & 1u))
#define AUDIO_DEVICE_CARD(volume_id) (((uint32_t)(volume_id) >> 8) & 0x00ffffffu)
#define AUDIO_DEVICE_INDEX(volume_id) (((uint32_t)(volume_id) >> 1) & 0x7fu)
#define AUDIO_DEVICE_DIRECTION(volume_id) ((enum audio_direction)((volume_id) & 1u))

struct audio_pcm_status {
    enum audio_pcm_state state;
    enum audio_direction direction;
    uint32_t frame_bytes;
    uint32_t buffer_frames;
    uint64_t avail_min;
    uint64_t boundary;
    uint64_t hw_ptr;
    uint64_t appl_ptr;
    int error;
    struct { int64_t tv_sec, tv_nsec; } tstamp;
    struct { int64_t tv_sec, tv_nsec; } trigger_tstamp;
};

enum audio_pcm_mmap_region {
    AUDIO_PCM_MMAP_DATA = 0,
    AUDIO_PCM_MMAP_STATUS = 1,
    AUDIO_PCM_MMAP_CONTROL = 2,
};

/* Linux v6.14 PCM mmap offsets. Keep these independent of the user header so
 * the core can validate mappings without importing the complete UAPI. */
#define AUDIO_PCM_MMAP_OFFSET_DATA         0x00000000ULL
#define AUDIO_PCM_MMAP_OFFSET_STATUS_OLD   0x80000000ULL
#define AUDIO_PCM_MMAP_OFFSET_CONTROL_OLD  0x81000000ULL
#define AUDIO_PCM_MMAP_OFFSET_STATUS_NEW   0x82000000ULL
#define AUDIO_PCM_MMAP_OFFSET_CONTROL_NEW  0x83000000ULL

struct audio_pcm_mmap {
    struct audio_pcm *pcm;
    enum audio_pcm_mmap_region region;
    uint32_t generation;
    uint32_t prot;
    uint64_t offset;
    uint64_t length;
    uint64_t backing;
};

struct audio_pcm;
struct snd_pcm_hw_params;
struct snd_pcm_channel_info;
struct snd_pcm_sw_params;
struct snd_pcm_info;
struct audio_pcm *audio_pcm_open(uint32_t card, uint32_t device,
                                 enum audio_direction direction, int *error);
/** @brief Copy the live PCM hardware capabilities for ALSA constraint setup.
 * @param pcm Live PCM object.
 * @param caps Writable capability output.
 * @return Zero on success, or -ENODEV/-EINVAL.
 */
int audio_pcm_caps(struct audio_pcm *pcm, struct audio_caps *caps);
/** @brief Validate and retain one PCM data/status/control mapping.
 * @param pcm Live PCM object with a selected hardware configuration.
 * @param offset Linux SNDRV_PCM_MMAP_OFFSET_* selector.
 * @param length Requested mapping length in bytes.
 * @param prot Linux PROT_READ/PROT_WRITE bits supplied by the VMA.
 * @param mapping Writable mapping metadata output.
 * @return Zero on success, or -EINVAL/-EPERM/-EBADFD/-ENODEV.
 */
int audio_pcm_mmap_acquire(struct audio_pcm *pcm, uint64_t offset,
                           uint64_t length, uint32_t prot,
                           struct audio_pcm_mmap *mapping);
/** @brief Retain existing VMA backing across fork or a metadata split.
 * @param pcm Borrowed PCM with a live mapping; caller owns the existing reference.
 * @param region Existing region selector, independent of current protection.
 * @return Zero or -EINVAL. Task context; IRQ-safe lock; release the new reference.
 */
int audio_pcm_mmap_retain(struct audio_pcm *pcm, enum audio_pcm_mmap_region region);
/** @brief Release one PCM mapping reference retained by audio_pcm_mmap_acquire. */
void audio_pcm_mmap_release(struct audio_pcm *pcm, enum audio_pcm_mmap_region region);
/** @brief Select a core-owned DMA ring in OPEN/SETUP/PREPARED.
 * @param pcm Live OFD-owned stream; serialized in task context.
 * @param params Borrowed validated hardware configuration.
 * @return Zero, -EBADFD for state/data mappings, or validation/device errno.
 * The caller retains ownership; IRQ-safe locks protect published counters.
 */
int audio_pcm_hw_params(struct audio_pcm *pcm, const struct audio_params *params);
/** @brief Query selected-format constraints.
 * @param pcm Borrowed live OFD PCM, serialized in task context.
 * @param caps Caller-owned output snapshot.
 * @return Zero or -EINVAL/-ENODEV; IRQ-safe lock; no ownership transfer.
 */
int audio_pcm_format_caps(struct audio_pcm *pcm, struct audio_format_caps *caps);
/** @brief Select exact memory format and DMA geometry.
 * @param pcm Borrowed OFD PCM, serialized in task context.
 * @param selection Borrowed validated format, subformat, precision and geometry.
 * @return Zero or state/device/geometry errno; IRQ-safe commit, no ownership transfer.
 */
int audio_pcm_hw_params_format(struct audio_pcm *pcm, const struct audio_params_ext *selection);
/** @brief Release a stopped PCM configuration while retaining its open lease.
 * @param pcm OFD-owned stream, serialized in task context; must be non-NULL.
 * @return Zero, -EBADFD unless SETUP/PREPARED without data mappings, or errno.
 * Hardware STOP precedes clearing the core ring; no backing page is freed.
 * A concurrent fatal disconnect returns -ENODEV even if STOP reports an error.
 */
int audio_pcm_hw_free(struct audio_pcm *pcm);

int audio_pcm_info(struct audio_pcm *pcm, struct snd_pcm_info *info);
int audio_pcm_sw_params(struct audio_pcm *pcm, struct snd_pcm_sw_params *params);
int audio_pcm_tstamp(struct audio_pcm *pcm, int type);
int audio_pcm_channel_info(struct audio_pcm *pcm, struct snd_pcm_channel_info *info);
/** @brief Refresh a running/draining PCM from its pinned driver pointer.
 * @param pcm Borrowed live PCM, serialized in task context; may be NULL.
 * @return Zero or state/device errno; fatal disconnect dominates callback error.
 * Queries outside the PCM lock, rechecks before publication, and never rewinds
 * an IRQ-observed pointer. PREPARED/PAUSED only validate state. No IRQ period is
 * synthesized; pointer error publishes XRUN and attempts STOP without freeing DMA.
 */
int audio_pcm_hwsync(struct audio_pcm *pcm);
long audio_pcm_move(struct audio_pcm *pcm, uint64_t frames, int forward);
/** @brief Prepare configured DMA without reviving a disconnected generation.
 * @param pcm Borrowed configured PCM, serialized in task context.
 * @return Zero or state/hardware errno; concurrent disconnect overrides callback errors with -ENODEV.
 */
int audio_pcm_prepare(struct audio_pcm *pcm);
/** @brief Start a PCM and its linked peer while preserving fatal metadata.
 * @param pcm Borrowed master PCM, serialized in task context.
 * @return Zero or start/rollback STOP/state errno; disconnect returns ENODEV.
 * Failed rollback STOP retains DMA and publishes XRUN for a safe retry.
 */
int audio_pcm_start(struct audio_pcm *pcm);
/** @brief Stop and reset a connected PCM while preserving fatal metadata.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @return Zero or state/hardware errno; concurrent disconnect overrides callback errors with -ENODEV.
 */
int audio_pcm_drop(struct audio_pcm *pcm);
/** @brief Reset application and hardware pointers without changing stream state. */
int audio_pcm_reset(struct audio_pcm *pcm);
/** @brief Force a connected stream into XRUN and stop its hardware engine.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @return STOP status or -ENODEV on disconnect, overriding other callback errors.
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
/** @brief Pause or resume while preserving a concurrent forced disconnect.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @param pause True for PAUSE, false for UNPAUSE.
 * @return Zero or state/hardware errno; concurrent disconnect overrides callback errors with -ENODEV.
 */
int audio_pcm_pause(struct audio_pcm *pcm, bool pause);
/** @brief Complete submitted playback at its final hardware period, or DROP capture.
 * @param pcm Live PCM reference held by caller.
 * @return 0 after STOP, -EAGAIN while draining, or negative errno.
 * Task context; failed STOP retains DRAINING and the borrowed DMA ring.
 * Fatal disconnect overrides callback errors with -ENODEV.
 */
int audio_pcm_drain(struct audio_pcm *pcm);
int audio_pcm_link(struct audio_pcm *master, struct audio_pcm *slave);
int audio_pcm_unlink(struct audio_pcm *pcm);
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
/** @brief Snapshot PCM state and frame pointers after polling hardware progress.
 * @param pcm Live PCM object.
 * @param status Writable status output.
 * @return Zero on success, or -ENODEV/-EINVAL.
 */
int audio_pcm_status(struct audio_pcm *pcm, struct audio_pcm_status *status);
/** @brief Drop the final OFD reference while retaining backing on failed STOP.
 * @param pcm Live PCM owned by the closing OFD; may be NULL.
 * @return None. Task context; failed closes remain reserved for manager retry.
 * Fatal DISCONNECTED state and ENODEV survive hardware close failures.
 */
void audio_pcm_release(struct audio_pcm *pcm);
/** @brief Inspect retained final releases without calling module code.
 * @return True while failed hardware closes need a task retry. IRQ safe.
 */
bool audio_pcm_release_work_pending(void);
/** @brief Retry a bounded number of retained final hardware closes.
 * @param budget Maximum PCM attempts, including failures.
 * @return Attempt count. Task context with manager admission and execution
 * ownership released. Successful closes stay reserved until finish_releases.
 */
uint32_t audio_pcm_retry_releases(uint32_t budget);
/** @brief Publish completed hardware-close retirements after execution resumes.
 * @return None. Task context under execution ownership; VMA refs retain backing.
 */
void audio_pcm_finish_releases(void);
/** @brief Intersect ALSA hardware parameters with the live PCM capabilities.
 * @param pcm Live PCM object.
 * @param params In/out Linux ALSA hardware constraint set.
 * @return Zero on a nonempty intersection, or -EINVAL/-ENODEV.
 */
int audio_alsa_refine(struct audio_pcm *pcm, struct snd_pcm_hw_params *params);
/** @brief Dispatch the supported Linux PCM ioctl subset on a live PCM.
 * @param pcm Live PCM object.
 * @param request Linux ALSA PCM ioctl number.
 * @param argument Kernel-accessible argument storage after user validation.
 * @return Zero, a short-transfer result in the argument, or a negative errno.
 */
int audio_alsa_ioctl(struct audio_pcm *pcm, uint64_t request, void *argument);

struct task;
struct task_file;
typedef void (*audio_pcm_notify_fn)(uint32_t stream, uint64_t frames, int error);
/** @brief Install the permanent built-in PCM metadata notification hook.
 * @param notify Non-sleeping kernel-resident hook, or NULL before PCM use.
 * @return None. Task context; never a module callback. The hook runs after
 * releasing the registry lock and has permanent text/storage lifetime.
 */
void audio_pcm_set_notifier(audio_pcm_notify_fn notify);
/** @brief Snapshot the IRQ PCM notification epoch before probing a transfer.
 * @return Wrapping epoch; task/IRQ safe. No reference or ownership transfer.
 */
uint32_t audio_pcm_event_epoch(void);
/** @brief Register the current PCM caller and publish interruptible sleep.
 * @return None. Task context under execution ownership; the pinned OFD remains
 * held across retry. Recheck the event epoch after publishing sleep. A full
 * wait queue leaves the caller runnable for the scheduler retry fallback.
 */
void audio_pcm_wait_current(void);
/** @brief Remove a completed or stale PCM wait on syscall entry.
 * @param task Borrowed task pinned by the execution transaction.
 * @return None. Task context; preserves scheduler state and OFD ownership.
 */
void audio_pcm_wait_remove(struct task *task);
/** @brief Parse a standard pcmC<N>D<M>{p,c} basename. */
int audio_device_parse_name(const char *name, uint32_t *card, uint32_t *device,
                            enum audio_direction *direction);
/** @brief Acquire an unconfigured PCM before publishing a standard descriptor.
 * @param task Calling task; currently unused by the core adapter.
 * @param file Descriptor/OFD storage containing the synthetic node identity.
 * @return Zero with the stream in OPEN, or a negative Linux errno. Task context;
 * execution ownership serializes the pool. The OFD owns the core reference.
 */
int audio_device_open(struct task *task, struct task_file *file);
/** @brief Validate a standard PCM mmap and return its core mapping metadata.
 * @param task Calling task; reserved for access-mode checks.
 * @param file Open PCM description.
 * @param offset Linux mmap offset selector.
 * @param length Page-aligned requested mapping length.
 * @param prot Linux PROT_READ/PROT_WRITE bits.
 * @param mapping Writable mapping metadata output.
 * @return Zero for supported data mapping, or a negative Linux errno.
 */
int audio_device_mmap(struct task *task, struct task_file *file,
                      uint64_t offset, uint64_t length, uint32_t prot,
                      struct audio_pcm_mmap *mapping);
/** @brief Release the final description's standard PCM core reference. */
int audio_device_close(struct task_file *file);
/** @brief Read complete or short captured frames from a standard PCM node. */
int audio_device_read(struct task *task, struct task_file *file,
                      void *buffer, uint32_t count);
/** @brief Write complete or short playback frames to a standard PCM node. */
int audio_device_write(struct task *task, struct task_file *file,
                       const void *buffer, uint32_t count);
/** @brief Dispatch a supported standard PCM ioctl request. */
int audio_device_ioctl(struct task *task, struct task_file *file,
                       uint64_t request, uint64_t address);
/** @brief Return poll readiness/error bits for a standard PCM description. */
short audio_device_poll(const struct task_file *file, short events);
/** @brief Reject seek because PCM nodes are frame streams. */
int audio_device_seek(struct task_file *file, int64_t offset, int whence,
                      uint64_t *result);

struct audio_timer_file;
/** @brief Open one ALSA PCM slave timer open-file description. */
int audio_timer_open(struct task *task, struct task_file *file);
/** @brief Close the final ALSA timer open-file description. */
int audio_timer_close(struct task_file *file);
/** @brief Read queued ALSA timer events into a kernel-accessible buffer. */
int audio_timer_read(struct task *task, struct task_file *file,
                     void *buffer, uint32_t count);
/** @brief Dispatch the Linux ALSA timer ioctl subset. */
int audio_timer_ioctl(struct task *task, struct task_file *file,
                      uint64_t request, uint64_t address);
/** @brief Return readiness/error bits for an ALSA timer description. */
short audio_timer_poll(const struct task_file *file, short events);
/** @brief Reject seek because ALSA timer nodes are event streams. */
int audio_timer_seek(struct task_file *file, int64_t offset, int whence,
                     uint64_t *result);

struct audio_control_file;
/** @brief Open a control OFD and register its preallocated event queue.
 * @param card Live generation token with at most AUDIO_CONTROL_EVENT_SLOTS elements.
 * @param out Writable output receiving owned OFD state on success.
 * @return Zero, -EINVAL, -ENODEV, -EOVERFLOW, or -ENFILE. Execution ownership
 * serializes the file pool; queue attachment separately excludes IRQ updates.
 */
int audio_control_open_card(uint32_t card, struct audio_control_file **out);
/** @brief Detach an OFD event queue and release its element locks.
 * @param file Live owned OFD released at its final description reference.
 * @return Zero or -EBADF. Execution ownership serializes lock/file tables;
 * queue detachment completes under the registry lock before storage reuse.
 */
int audio_control_close_file(struct audio_control_file *file);
/** @brief Dispatch an ALSA control ioctl through shared OFD state.
 * @param file Live control OFD, serialized by execution ownership.
 * @param request Linux ioctl number; unsupported operations return explicit errors.
 * @param argument Kernel-accessible storage, including validated nested buffers.
 * @return Zero or a negative Linux errno; successful writes notify subscribers.
 * SUBSCRIBE_EVENTS reads back state for negative inputs and preserves queued
 * events on repeated enable. ELEM_INFO reports LOCK only while locked, OWNER
 * only to that OFD, and owner=-1 when unlocked, matching Linux control semantics.
 */
int audio_alsa_control_ioctl(struct audio_control_file *file, uint64_t request,
                             void *argument);
/** @brief Write a shared control through the mixer/OFD lock boundary. */
int audio_control_write_shared(uint32_t card, uint32_t control,
                               const struct audio_control_value *value);
/** @brief Copy and consume one pending event from a subscribed control OFD.
 * @param file Borrowed live control description, serialized by execution ownership.
 * @param buffer Kernel-accessible output for one snd_ctl_event.
 * @param count Output capacity in bytes.
 * @return Event bytes, -EBADFD if unsubscribed, -EAGAIN if empty, or negative errno.
 * Does not sleep; the task-aware wrapper owns blocking and signal wakeups.
 */
int audio_control_read_file(struct audio_control_file *file, void *buffer,
                            uint32_t count);
/** @brief Read an event or publish a race-checked interruptible control wait.
 * @param task Current task pinned by the syscall execution transaction.
 * @param file Borrowed control OFD, retained across syscall retries.
 * @param buffer Kernel-accessible output for one snd_ctl_event.
 * @param count Output capacity in bytes.
 * @return Event bytes, negative errno, or KERNEL_SYSCALL_BLOCKED for retry.
 * Queue exhaustion uses internal -EAGAIN; O_NONBLOCK callers use read_file.
 */
int audio_control_read_task(struct task *task, struct task_file *file,
                            void *buffer, uint32_t count);
/** @brief Inspect pending control events without consuming them.
 * @param file Borrowed live control OFD, serialized by execution ownership.
 * @param events Requested poll bits; only read readiness is produced.
 * @return Read bits for pending events, POLLNVAL for an invalid OFD, or
 * POLLERR|POLLHUP for a disconnected card. Does not allocate or sleep.
 */
short audio_control_poll_file(const struct audio_control_file *file, short events);
/** @brief Open a task file backed by /dev/snd/controlC*. */
int audio_control_open(struct task *task, struct task_file *file);
/** @brief Close a task file backed by /dev/snd/controlC*. */
int audio_control_close(struct task_file *file);
/** @brief Dispatch a user-facing control ioctl after validating its argument. */
int audio_control_ioctl(struct task *task, struct task_file *file,
                        uint64_t request, uint64_t address);
struct audio_oss_file;
/** @brief Open one OSS DSP open-file description with portable defaults. */
int audio_oss_open(struct task *task, struct task_file *file);
/** @brief Close and release one OSS DSP open-file description. */
int audio_oss_close(struct task_file *file);
/** @brief Read capture bytes from an OSS DSP description. */
int audio_oss_read(struct task *task, struct task_file *file,
                   void *buffer, uint32_t count);
/** @brief Write playback bytes to an OSS DSP description. */
int audio_oss_write(struct task *task, struct task_file *file,
                    const void *buffer, uint32_t count);
/** @brief Dispatch a checked OSS DSP ioctl. */
int audio_oss_ioctl(struct task *task, struct task_file *file,
                    uint64_t request, uint64_t address);
/** @brief Return OSS DSP poll readiness for one open-file description. */
short audio_oss_poll(const struct task_file *file, short events);
struct audio_mixer_file;
/** @brief Open one OSS mixer state for a generation-tagged card. */
int audio_mixer_open_card(uint32_t card, struct audio_mixer_file **out);
/** @brief Close one OSS mixer open-file-description state. */
int audio_mixer_close_file(struct audio_mixer_file *file);
/** @brief Dispatch one kernel-accessible OSS mixer ioctl. */
int audio_mixer_ioctl_file(struct audio_mixer_file *file, uint64_t request,
                           void *argument);
/** @brief Open a /dev/mixer task file. */
int audio_mixer_open(struct task *task, struct task_file *file);
/** @brief Close a /dev/mixer task file. */
int audio_mixer_close(struct task_file *file);
/** @brief Dispatch a checked OSS mixer ioctl. */
int audio_mixer_ioctl(struct task *task, struct task_file *file,
                      uint64_t request, uint64_t address);
/** @brief Return mixer output readiness/error bits. */
short audio_mixer_poll(const struct task_file *file, short events);
#endif

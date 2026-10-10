#include <linux/errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <reliefnt/audio.h>
#include "hda.h"

/* Controller/stream-only fixtures do not link codec.c; production supplies the
 * strong task-context converter binding implementation. */
__attribute__((weak)) int hda_route_group_stream_bind(struct hda_route_group *group,
                                                       uint8_t tag, uint16_t format)
{
#ifdef HDA_STREAM_TESTING
    extern int hda_stream_test_route_bind(struct hda_route_group *, uint8_t,
                                          uint16_t);
    return hda_stream_test_route_bind(group, tag, format);
#else
    (void)group;
    (void)tag;
    (void)format;
    return 0;
#endif
}

#define HDA_STREAM_BASE_REG 0x80u
#define HDA_STREAM_STRIDE_REG 0x20u
#define HDA_REG_INTCTL_REG 0x20u
#define HDA_REG_INTSTS_REG 0x24u
#define HDA_REG_WALLCLK 0x30u
#define HDA_WALLCLK_HZ UINT64_C(24000000)
#define HDA_SDCTL 0x00u
#define HDA_SDSTS 0x03u
#define HDA_SDLPIB 0x04u
#define HDA_SDCBL 0x08u
#define HDA_SDLVI 0x0cu
#define HDA_SDFMT 0x12u
#define HDA_SDBDPL 0x18u
#define HDA_SDBDPU 0x1cu
#define HDA_REG_DPLBASE 0x70u
#define HDA_REG_DPUBASE 0x74u
#define HDA_DPLBASE_ENABLE 1u
#define HDA_SDCTL_SRST (1u << 0)
#define HDA_SDCTL_RUN (1u << 1)
#define HDA_SDCTL_IOCE (1u << 2)
#define HDA_SDCTL_FEIE (1u << 3)
#define HDA_SDCTL_DEIE (1u << 4)
#define HDA_SDCTL_DIR (1u << 19)
#define HDA_SDCTL_TAG_SHIFT 20u
#define HDA_SDCTL_TAG_MASK (0x0fu << HDA_SDCTL_TAG_SHIFT)
#define HDA_SDSTS_BCIS (1u << 2)
#define HDA_SDSTS_FIFOE (1u << 3)
#define HDA_SDSTS_DESE (1u << 4)

/** @brief Compute one stream register offset from its slot index. */
static uint32_t hda_stream_offset(const struct hda_stream *s, uint32_t reg)
{
    return HDA_STREAM_BASE_REG + s->index * HDA_STREAM_STRIDE_REG + reg;
}

/** @brief Select the inclusive DMA bus mask from GCAP. */
static uint64_t hda_dma_mask(const struct hda_controller *c)
{
    return (c->gcap & 1u) ? UINT64_MAX : UINT32_MAX;
}

/** @brief Validate and recover the private stream from a core lease. */
static struct hda_stream *hda_stream_from_hw(struct audio_hw_stream *stream)
{
    if (!stream || !stream->driver) return NULL;
    struct hda_stream *s = stream->driver;
    return s->id == stream->id ? s : NULL;
}

/** @brief Report whether a route group still has converter state to release.
 * @param group Route group retained by the stream lease.
 * @return True when any converter assignment or retry marker remains.
 *
 * @par Context
 * Task context while closing a stream; no codec verbs are issued.
 * @par Ownership and locks/IRQ
 * Borrows immutable route metadata under the caller's stream ownership; no lock
 * or interrupt state is changed.
 */
static bool hda_stream_group_needs_release(const struct hda_route_group *group)
{
    if (!group || !group->members) return false;
    for (uint32_t i = 0; i < group->member_count; ++i) {
        const struct hda_route *route = &group->members[i];
        if (route->stream_bound || route->stream_bind_dirty ||
            route->stream_tag || route->stream_format) return true;
    }
    return false;
}

/** @brief Serialize position readers against IRQ updates without sleeping.
 * @param s Live stream metadata.
 * @return Saved interrupt flags, to restore after the position update.
 * Task/IRQ context; the short private lock owns no callback or hardware wait.
 */
static uint64_t hda_stream_position_lock(struct hda_stream *s)
{
    uint64_t flags;
#ifdef HDA_TESTING
    flags = hda_test_irq_save();
#else
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(&s->position_lock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    return flags;
}

/** @brief Release the stream position lock and restore entry interrupt state.
 * @param s Locked stream.
 * @param flags Saved interrupt flags.
 * @return None. Task/IRQ context; no allocation, wait, or callback.
 */
static void hda_stream_position_unlock(struct hda_stream *s, uint64_t flags)
{
    __atomic_store_n(&s->position_lock, 0u, __ATOMIC_RELEASE);
#ifdef HDA_TESTING
    hda_test_irq_restore(flags);
#else
    if (flags & (UINT64_C(1) << 9)) __asm__ volatile("sti" ::: "memory");
#endif
}

/** @brief Report a rejected hardware position after releasing its private lock.
 * @param s Borrowed prepared stream, unchanged by this diagnostic.
 * @param last Last DMA position captured under the position lock.
 * @param wall_prev Previous WALLCLK captured under the position lock.
 * @param reason Static rejection reason.
 * @param position Observed DMA byte offset.
 * @param before First WALLCLK observation. @param after Last observation.
 * @return None. Task/IRQ error path only; bounded stack formatting, no allocation.
 */
static void hda_stream_position_error(const struct hda_stream *s,const char *reason,
                                      uint32_t position,uint32_t before,uint32_t after,
                                      uint32_t last,uint32_t wall_prev)
{
    const struct reliefos_driver_kernel_api *api=s->controller->api;
    if(!api || !api->console_write)return;
    char line[256];unsigned n=0;const char *text="[hda-pos] reason=";
    while(*text)line[n++]=*text++;
    while(*reason)line[n++]=*reason++;
    const char *labels[]={" sd="," last="," pos="," cbl="," wall_prev=",
                          " wall_before="," wall_after="," frames="};
    uint32_t values[]={s->index,last,position,s->cbl,
                       wall_prev,before,after,s->selected.pcm.buffer_frames};
    const char *hex="0123456789abcdef";
    for(unsigned i=0;i<8;i++){
        text=labels[i];while(*text)line[n++]=*text++;
        line[n++]='0';line[n++]='x';
        for(int shift=28;shift>=0;shift-=4)line[n++]=hex[(values[i]>>shift)&15u];
    }
    line[n++]='\n';line[n]=0;api->console_write(line);
}

/** @brief Advance one hardware byte position into a monotonic frame count.
 * @param s Prepared stream whose position state is advanced.
 * @param frames Receives the resulting monotonic frame count.
 * @return 0 or -EPIPE for invalid geometry or a gap admitting multiple wraps.
 *
 * @par Context
 * Pointer polling or bounded stream IRQ service; no allocation or waiting.
 * @par Ownership and locks/IRQ
 * Caller owns the stream lease. A private IRQ-safe lock serializes pointer and
 * service observations, including the hardware position read bracketed by the
 * 24 MHz WALLCLK. No callback or hardware wait occurs under this lock.
 */
static int hda_stream_position_update(struct hda_stream *s, uint64_t *frames)
{
    if (!s || !frames || !s->controller || !s->selected.pcm.frame_bytes ||
        !s->selected.pcm.rate || !s->cbl) return -EPIPE;
    uint64_t flags = hda_stream_position_lock(s);
    struct hda_controller *c = s->controller;
    uint64_t tick = c->api && c->api->ticks ? c->api->ticks() : s->position_tick;
    uint32_t before = hda_mmio_read32(c, HDA_REG_WALLCLK);
    uint32_t position = s->position ? __atomic_load_n(s->position, __ATOMIC_ACQUIRE) :
                        hda_mmio_read32(c, hda_stream_offset(s, HDA_SDLPIB));
    uint32_t after = hda_mmio_read32(c, HDA_REG_WALLCLK);
    uint32_t last = s->last_position, wall_prev = s->position_wallclock;
    if (position > s->cbl || position % s->selected.pcm.frame_bytes) {
        hda_stream_position_unlock(s, flags);
        hda_stream_position_error(s,"invalid",position,before,after,last,wall_prev);
        return -EPIPE;
    }
    if (s->state == HDA_STREAM_RUNNING && s->position_valid) {
        /* WALLCLK brackets link-clock samples. One cycle covers quantization;
         * a full ring is ambiguous even if LPIB is unchanged. System timer
         * delivery can bunch up independently of link/DMA progress, so coarse
         * ticks only reject gaps that can alias the entire 32-bit WALLCLK. */
        uint64_t elapsed = (uint32_t)(after - s->position_wallclock);
        uint64_t tick_gap = tick - s->position_tick;
        uint64_t wallclock_ticks = ((UINT64_C(1) << 32) * 100u +
                                   HDA_WALLCLK_HZ - 1u) / HDA_WALLCLK_HZ;
        bool coarse_gap = tick_gap > 1u && tick_gap - 1u >= wallclock_ticks;
        if (coarse_gap || (elapsed + 1u) * s->selected.pcm.rate >=
                          (uint64_t)s->selected.pcm.buffer_frames * HDA_WALLCLK_HZ) {
            hda_stream_position_unlock(s, flags);
            hda_stream_position_error(s,"gap",position,before,after,last,wall_prev);
            return -EPIPE;
        }
    }
    s->position_tick = tick;
    s->position_wallclock = before;
    /* Hardware normally wraps directly to zero; CBL itself is an optional
     * boundary observation and must not count a second wrap at the next zero. */
    if (position == s->cbl) position = 0u;
    if (s->position_valid) {
        if (position == s->last_position) {
            *frames = s->frame_position;
            hda_stream_position_unlock(s, flags);
            return 0;
        }
        if (position < s->last_position) {
            s->position_epoch += s->cbl / s->selected.pcm.frame_bytes;
        }
    } else {
        s->position_valid = 1u;
    }
    uint64_t completed = s->position_epoch +
                         position / s->selected.pcm.frame_bytes;
    if (completed < s->frame_position) {
        hda_stream_position_unlock(s, flags);
        hda_stream_position_error(s,"backward",position,before,after,last,wall_prev);
        return -EPIPE;
    }
    s->last_position = position;
    s->frame_position = completed;
    *frames = completed;
    hda_stream_position_unlock(s, flags);
    return 0;
}

/** @brief Map converter precision to the HDA format width code. */
static uint32_t hda_stream_format_bits(uint32_t bits)
{
    switch (bits) {
    case 8: return 0u;
    case 16: return 1u;
    case 20: return 2u;
    case 24: return 3u;
    case 32: return 4u;
    default: return UINT32_MAX;
    }
}

/** @brief Encode one HDA stream format word using exact integer rate factors.
 * @param rate Integer sample rate.
 * @param bits Converter precision (8, 16, 20, 24, or 32).
 * @param channels Channel count from 1 through 16.
 * @param out Receives the encoded descriptor value.
 * @return 0, -EINVAL for bad arguments, or -ERANGE for an unsupported rate.
 * Pure conversion; safe in IRQ context with no ownership or waiting.
 */
int hda_format_encode(uint32_t rate, uint32_t bits, uint32_t channels,
                      uint16_t *out)
{
    if (!out || !channels || channels > 16u) return -EINVAL;
    uint32_t width = hda_stream_format_bits(bits);
    if (width == UINT32_MAX) return -EINVAL;
    if (rate != 44100u && rate != 48000u) {
        for (uint32_t base_index = 0u; base_index < 2u; ++base_index) {
            uint32_t base = base_index ? 44100u : 48000u;
            for (uint32_t mult = 1u; mult <= 8u; ++mult) {
                for (uint32_t div = 1u; div <= 8u; ++div) {
                    if (mult > 4u || div > 4u) continue;
                    if ((base * mult) / div != rate || (base * mult) % div) continue;
                    *out = (uint16_t)(((base == 44100u) ? 0x4000u : 0u) |
                                      ((mult - 1u) << 11) | ((div - 1u) << 8) |
                                      (width << 4) | (channels - 1u));
                    return 0;
                }
            }
        }
        return -ERANGE;
    }
    *out = (uint16_t)(((rate == 44100u) ? 0x4000u : 0u) |
                      (width << 4) | (channels - 1u));
    return 0;
}

/** @brief Allocate persistent stream descriptor and position DMA runs.
 * @param c Initialized controller with H1 DMA callbacks.
 * @param table Caller-owned stream metadata array.
 * @param count Number of stream slots.
 * @return 0 or negative errno; partial allocation is synchronously reclaimed.
 * Task context only; no stream callbacks or controller waits are performed.
 */
int hda_stream_controller_init(struct hda_controller *c,
                               struct hda_stream *table, uint32_t count)
{
    if (!c || !c->api || !c->api->alloc_dma || !c->api->free_dma ||
        !table || !count || count > HDA_STREAM_MAX || c->streams) return -EINVAL;
    uint64_t mask = hda_dma_mask(c);
    uint32_t position_pages = (count * 8u + 4095u) / 4096u;
    uint64_t position = c->api->alloc_dma(position_pages, mask);
    if (!position) return -ENOMEM;
    c->position_phys = position;
    c->position_pages = position_pages;
    c->position_buffer = hda_phys_to_direct_map_page(position);
    if (!c->position_buffer) {
        c->api->free_dma(position, position_pages);
        c->position_phys = 0;
        return -EFAULT;
    }
    __builtin_memset((void *)c->position_buffer, 0, position_pages * 4096u);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t phys = c->api->alloc_dma(1u, mask);
        if (!phys || (phys & 127u)) {
            if (phys) c->api->free_dma(phys, 1u);
            for (uint32_t j = 0; j < i; ++j)
                c->api->free_dma(table[j].bdl_dma.bus, 1u);
            c->api->free_dma(position, position_pages);
            c->position_phys = 0;
            c->position_buffer = NULL;
            return phys ? -EINVAL : -ENOMEM;
        }
        __builtin_memset(&table[i], 0, sizeof(table[i]));
        table[i].controller = c;
        table[i].index = i;
        table[i].bdl_dma.bus = phys;
        table[i].bdl_dma.bytes = 4096u;
        table[i].bdl_dma.kernel = hda_phys_to_direct_map_page(phys);
        if (!table[i].bdl_dma.kernel) {
            c->api->free_dma(phys, 1u);
            for (uint32_t j = 0; j < i; ++j)
                c->api->free_dma(table[j].bdl_dma.bus, 1u);
            c->api->free_dma(position, position_pages);
            c->position_phys = 0u;
            c->position_buffer = NULL;
            return -EFAULT;
        }
        table[i].bdl = table[i].bdl_dma.kernel;
        table[i].position_dma.bus = position + i * 8u;
        table[i].position_dma.bytes = 8u;
        table[i].position_dma.kernel = (void *)(uintptr_t)(c->position_buffer + i * 8u);
        table[i].position = (volatile uint32_t *)table[i].position_dma.kernel;
        table[i].state = HDA_STREAM_FREE;
    }
    c->streams = table;
    c->stream_count = count;
    c->owns_stream_dma = 1u;
    c->stream_tag_mask = 0u;
    c->stream_irq_mask = 0u;
    hda_mmio_write32(c, HDA_REG_DPLBASE,
                     (uint32_t)(position & ~UINT64_C(0x7f)) | HDA_DPLBASE_ENABLE);
    hda_mmio_write32(c, HDA_REG_DPUBASE, (uint32_t)(position >> 32));
    return 0;
}

/** @brief Release persistent stream DMA after all stream leases are stopped.
 * @param c Controller with a stream table.
 * @return 0, -EBUSY while a stream is live, -EIO while DMA cannot be stopped,
 * or -EINVAL for missing ownership.
 * Task context only; failed quiescence retains every DMA lease for retry.
 */
int hda_stream_controller_destroy(struct hda_controller *c)
{
    if (!c || !c->streams || !c->owns_stream_dma) return -EINVAL;
    for (uint32_t i = 0; i < c->stream_count; ++i)
        if (c->streams[i].state != HDA_STREAM_FREE) return -EBUSY;
    for (uint32_t i = 0; i < c->stream_count; ++i)
        if (hda_mmio_read32(c, hda_stream_offset(&c->streams[i], HDA_SDCTL)) &
            HDA_SDCTL_RUN) return -EIO;
    /* The controller may update the position buffer even with every stream
     * stopped. Disable and verify that DMA writer before freeing its page. */
    hda_mmio_write32(c, HDA_REG_DPLBASE, 0u);
    if (hda_mmio_read32(c, HDA_REG_DPLBASE) & HDA_DPLBASE_ENABLE) return -EIO;
    hda_mmio_write32(c, HDA_REG_DPUBASE, 0u);
    for (uint32_t i = 0; i < c->stream_count; ++i)
        c->api->free_dma(c->streams[i].bdl_dma.bus, 1u);
    c->api->free_dma(c->position_phys, c->position_pages);
    c->streams = NULL;
    c->stream_count = 0u;
    c->position_phys = 0u;
    c->position_buffer = NULL;
    c->position_pages = 0u;
    c->owns_stream_dma = 0u;
    return 0;
}

/** @brief Query HDA native formats and implemented BDL geometry.
 * @param opaque Controller context.
 * @param device Stream slot index.
 * @param direction Playback/capture selector.
 * @param caps Receives selected format/subformat metadata and alignment bounds.
 * @return 0 or a negative argument/device errno. Task context only.
 */
int hda_pcm_format_caps(void *opaque, uint32_t device,
                        enum audio_direction direction,
                        struct audio_format_caps *caps)
{
    struct hda_controller *c = opaque;
    if (!c || !caps || (uint32_t)direction > AUDIO_CAPTURE ||
        !c->streams || device >= c->stream_count) return -EINVAL;
    __builtin_memset(caps, 0, sizeof(*caps));
    caps->pcm.formats = AUDIO_FORMAT_S16_LE | AUDIO_FORMAT_S32_LE;
    caps->pcm.rates = AUDIO_RATE_44100 | AUDIO_RATE_48000;
    caps->pcm.channels_min = 1u;
    caps->pcm.channels_max = 16u;
    caps->pcm.period_bytes_min = 128u;
    caps->pcm.period_bytes_max = UINT32_MAX & ~127u;
    caps->pcm.buffer_bytes_max = UINT32_MAX & ~127u;
    caps->format_step_bytes = 128u;
    caps->period_count_min = 2u;
    caps->period_count_max = HDA_BDL_MAX;
    caps->format_bits[0] = AUDIO_FORMAT_S16_LE;
    caps->subformats[0] = 1u << AUDIO_SUBFORMAT_STD;
    caps->format_bits[1] = AUDIO_FORMAT_S32_LE;
    caps->subformats[1] = (1u << AUDIO_SUBFORMAT_STD) |
                          (1u << AUDIO_SUBFORMAT_MSBITS_20) |
                          (1u << AUDIO_SUBFORMAT_MSBITS_24) |
                          (1u << AUDIO_SUBFORMAT_MSBITS_MAX);
    return 0;
}

/** @brief Return the legacy capability prefix from H4's bounded metadata.
 * @param opaque Controller context.
 * @param device Stream slot/index.
 * @param direction Playback/capture selector.
 * @param caps Receives the unchanged legacy capability fields.
 * @return 0 or a negative errno. Task context, no ownership transfer.
 */
int hda_pcm_caps(void *opaque, uint32_t device, enum audio_direction direction,
                 struct audio_caps *caps)
{
    if (!caps) return -EINVAL;
    struct audio_format_caps extended;
    int ret = hda_pcm_format_caps(opaque, device, direction, &extended);
    if (!ret) *caps = extended.pcm;
    return ret;
}

/** @brief Acquire one free stream slot and a unique HDA stream tag.
 * @param opaque Controller context.
 * @param device Stream slot index.
 * @param direction Playback/capture selector.
 * @param stream Receives the generation-tagged lease.
 * @return 0 or a negative errno. Task context; no allocation or codec wait.
 */
int hda_pcm_open(void *opaque, uint32_t device, enum audio_direction direction,
                 struct audio_hw_stream *stream)
{
    struct hda_controller *c = opaque;
    if (!c || !stream || (uint32_t)direction > AUDIO_CAPTURE ||
        !c->streams || device >= c->stream_count) return -EINVAL;
    struct hda_stream *s = &c->streams[device];
    if (s->state != HDA_STREAM_FREE) return -EBUSY;
    uint8_t tag = 0u;
    for (uint8_t i = 1u; i < 16u; ++i)
        if (!(c->stream_tag_mask & (uint16_t)(1u << i))) { tag = i; break; }
    if (!tag) return -ENOSPC;
    int ret = hda_controller_stream_acquire(c);
    if (ret) return ret;
    __builtin_memset(&s->pcm_dma, 0, sizeof(s->pcm_dma));
    s->controller = c;
    s->direction = (uint8_t)direction;
    s->tag = tag;
    c->stream_tag_mask |= (uint16_t)(1u << tag);
    s->card_id = c->card_id;
    s->id = ++c->stream_generation;
    if (!s->id) s->id = ++c->stream_generation;
    s->state = HDA_STREAM_OPEN;
    s->configured = 0u;
    s->xrun_reported = 0u;
    stream->id = s->id;
    stream->driver = s;
    return 0;
}

/** @brief Stop/reset a stream and program direction, BDL, CBL, LVI and format.
 * @param s Prepared stream metadata with a validated BDL.
 * @param format Encoded HDA format word.
 * @return 0 or -EIO when reset/run readback fails; no allocation or wait.
 */
static int hda_stream_reset_program(struct hda_stream *s, uint16_t format)
{
    struct hda_controller *c = s->controller;
    uint32_t reg = hda_stream_offset(s, HDA_SDCTL);
    uint32_t control = hda_mmio_read32(c, reg);
    control &= ~HDA_SDCTL_RUN;
    hda_mmio_write32(c, reg, control);
    if (hda_mmio_read32(c, reg) & HDA_SDCTL_RUN) return -EIO;
    hda_mmio_write8(c, hda_stream_offset(s, HDA_SDSTS),
                    HDA_SDSTS_BCIS | HDA_SDSTS_FIFOE | HDA_SDSTS_DESE);
    hda_mmio_write32(c, reg, control | HDA_SDCTL_SRST);
    if (!(hda_mmio_read32(c, reg) & HDA_SDCTL_SRST)) return -EIO;
    hda_mmio_write32(c, reg, control & ~HDA_SDCTL_SRST);
    if (hda_mmio_read32(c, reg) & HDA_SDCTL_SRST) return -EIO;
    control &= ~(HDA_SDCTL_DIR | HDA_SDCTL_TAG_MASK);
    if (s->direction == AUDIO_CAPTURE) control |= HDA_SDCTL_DIR;
    control |= ((uint32_t)s->tag << HDA_SDCTL_TAG_SHIFT);
    hda_mmio_write32(c, reg, control);
    hda_mmio_write32(c, hda_stream_offset(s, HDA_SDCBL), s->cbl);
    hda_mmio_write16(c, hda_stream_offset(s, HDA_SDLVI), (uint16_t)s->lvi);
    hda_mmio_write16(c, hda_stream_offset(s, HDA_SDFMT), format);
    hda_mmio_write32(c, hda_stream_offset(s, HDA_SDBDPL),
                     (uint32_t)s->bdl_dma.bus);
    hda_mmio_write32(c, hda_stream_offset(s, HDA_SDBDPU),
                     (uint32_t)(s->bdl_dma.bus >> 32));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return 0;
}

/** @brief Prepare the legacy S16 path by constructing an explicit selection.
 * @param opaque Controller context.
 * @param stream Borrowed open stream lease.
 * @param params Legacy PCM parameters; only unambiguous S16 is accepted.
 * @param dma Core-owned PCM DMA ring borrowed for this configuration.
 * @return 0 or a negative validation/programming errno; RUN remains clear.
 */
int hda_pcm_prepare(void *opaque, struct audio_hw_stream *stream,
                    const struct audio_params *params,
                    const struct audio_dma *dma)
{
    (void)opaque;
    if (!params) return -EINVAL;
    if (params->sample_bits != 16u) return -EOPNOTSUPP;
    struct audio_params_ext ext = {.pcm = *params, .format = AUDIO_FORMAT_S16_LE,
                                   .subformat = AUDIO_SUBFORMAT_STD,
                                   .significant_bits = 16u};
    return hda_pcm_prepare_format(opaque, stream, &ext, dma);
}

/** @brief Prepare an explicit format, BDL, converter word, and generation.
 * @param opaque Controller context.
 * @param stream Borrowed open lease.
 * @param params Selected memory format/subformat and significant precision.
 * @param dma Core-owned contiguous PCM ring borrowed until STOP/close.
 * @return 0 or a negative validation/programming errno; no RUN transition.
 *
 * @par Context
 * Task context only; converter binding is completed before BDL or SD MMIO is
 * published, and any later programming error releases that binding.
 * @par Ownership and locks/IRQ
 * Caller retains the stream lease and DMA ring; this path performs task-context
 * codec verbs and MMIO programming without holding an IRQ/service lock.
 */
int hda_pcm_prepare_format(void *opaque, struct audio_hw_stream *stream,
                           const struct audio_params_ext *params,
                           const struct audio_dma *dma)
{
    struct hda_controller *c = opaque;
    struct hda_stream *s = hda_stream_from_hw(stream);
    if (!c || !s || s->controller != c || !params || !dma ||
        (s->state != HDA_STREAM_OPEN && s->state != HDA_STREAM_STOPPED)) return -EINVAL;
    if (!dma->kernel || !dma->bus || (dma->bus & 127u)) return -EINVAL;
    if (params->pcm.channels < 1u || params->pcm.channels > 16u ||
        params->pcm.period_frames < 1u || params->pcm.buffer_frames < params->pcm.period_frames)
        return -EINVAL;
    uint32_t container = params->format == AUDIO_FORMAT_S16_LE ? 2u :
                         params->format == AUDIO_FORMAT_S32_LE ? 4u : 0u;
    if (!container || params->pcm.frame_bytes != params->pcm.channels * container)
        return -EINVAL;
    if (params->format == AUDIO_FORMAT_S16_LE &&
        (params->subformat != AUDIO_SUBFORMAT_STD || params->significant_bits != 16u))
        return -EINVAL;
    if (params->format == AUDIO_FORMAT_S32_LE &&
        ((params->subformat == AUDIO_SUBFORMAT_STD && params->significant_bits != 32u) ||
         (params->subformat == AUDIO_SUBFORMAT_MSBITS_20 && params->significant_bits != 20u) ||
         (params->subformat == AUDIO_SUBFORMAT_MSBITS_24 && params->significant_bits != 24u) ||
         (params->subformat == AUDIO_SUBFORMAT_MSBITS_MAX && params->significant_bits != 32u) ||
         params->subformat > AUDIO_SUBFORMAT_MSBITS_MAX)) return -EINVAL;
    if (params->pcm.rate != 44100u && params->pcm.rate != 48000u) return -ERANGE;
    if (s->group) {
        if (!s->group->active || params->pcm.channels != s->group->total_channels)
            return -EBUSY;
        for (uint32_t i = 0; i < s->group->member_count; ++i) {
            if (s->group->members[i].stream_bound) return -EBUSY;
            if (!s->group->members[i].channel_count ||
                s->group->members[i].channel_start + s->group->members[i].channel_count > 16u)
                return -EINVAL;
        }
    }
    uint64_t period_bytes64 = (uint64_t)params->pcm.period_frames * params->pcm.frame_bytes;
    uint64_t buffer_bytes64 = (uint64_t)params->pcm.buffer_frames * params->pcm.frame_bytes;
    if (period_bytes64 < 128u || period_bytes64 > UINT32_MAX || (period_bytes64 & 127u) ||
        buffer_bytes64 > UINT32_MAX || buffer_bytes64 != period_bytes64 *
        (params->pcm.buffer_frames / params->pcm.period_frames) ||
        params->pcm.buffer_frames % params->pcm.period_frames) return -EINVAL;
    uint32_t periods = params->pcm.buffer_frames / params->pcm.period_frames;
    if (periods < 2u || periods > HDA_BDL_MAX || dma->bytes < buffer_bytes64) return -EINVAL;
    uint16_t format;
    int ret = hda_format_encode(params->pcm.rate, params->significant_bits,
                                params->pcm.channels, &format);
    if (ret) return ret;
    bool reacquired = false;
    bool group_bound = false;
    if (s->state == HDA_STREAM_STOPPED) {
        ret = hda_controller_stream_acquire(c);
        if (ret) return ret;
        reacquired = true;
    }
    if (!s->bdl || !s->position) {
        ret = -EFAULT;
        goto prepare_fail;
    }
    if (s->group) {
        ret = hda_route_group_stream_bind(s->group, s->tag, format);
        if (ret) goto prepare_fail;
        group_bound = true;
    }
    s->period_bytes = (uint32_t)period_bytes64;
    s->period_count = periods;
    s->cbl = (uint32_t)buffer_bytes64;
    s->lvi = periods - 1u;
    s->bdl_entries = periods;
    s->pcm_dma = *dma;
    for (uint32_t i = 0; i < periods; ++i) {
        uint64_t address = dma->bus + (uint64_t)i * s->period_bytes;
        uint64_t end = address + (uint64_t)s->period_bytes - 1u;
        if ((address & 127u) || end < address ||
            ((c->gcap & 1u) == 0u && end > UINT32_MAX)) {
            ret = -EINVAL;
            goto prepare_fail;
        }
    }
    for (uint32_t i = 0; i < periods; ++i) {
        uint64_t address = dma->bus + (uint64_t)i * s->period_bytes;
        s->bdl[i].address = address;
        s->bdl[i].length = s->period_bytes;
        s->bdl[i].flags = HDA_BDL_IOC;
    }
    *s->position = 0u;
    s->selected = *params;
    s->last_position = 0u;
    s->last_period = 0u;
    s->position_epoch = 0u;
    s->frame_position = 0u;
    s->reported_frames = 0u;
    s->position_valid = 1u;
    s->hw_position = 0u;
    s->generation++;
    s->xrun_reported = 0u;
    ret = hda_stream_reset_program(s, format);
    if (ret) goto prepare_fail;
    if (s->group) {
        for (uint32_t i = 0; i < s->group->member_count; ++i) {
            if (s->group->members[i].stream_bound) {
                ret = -EBUSY;
                goto prepare_fail;
            }
            s->group->members[i].stream_tag = s->tag;
            s->group->members[i].stream_format = format;
            s->group->members[i].stream_bound = 1u;
        }
    }
    s->configured = 1u;
    s->state = HDA_STREAM_PREPARED;
    return 0;

prepare_fail:
    if (group_bound && hda_route_group_stream_bind(s->group, 0u, 0u) && !ret)
        ret = -EIO;
    if (reacquired) (void)hda_controller_stream_release(c);
    return ret;
}

/** @brief Trigger a stream while enforcing RUN/readback and stop retention.
 * @param opaque Controller context.
 * @param stream Borrowed stream lease.
 * @param trigger START, STOP, PAUSE, or UNPAUSE.
 * @return 0 or negative errno. Allocation-free and no codec waits.
 */
int hda_pcm_trigger(void *opaque, struct audio_hw_stream *stream,
                    enum audio_trigger trigger)
{
    struct hda_controller *c = opaque;
    struct hda_stream *s = hda_stream_from_hw(stream);
    if (!c || !s || s->controller != c) return -EINVAL;
    uint32_t reg = hda_stream_offset(s, HDA_SDCTL);
    uint32_t control = hda_mmio_read32(c, reg);
    if (trigger == AUDIO_START || trigger == AUDIO_UNPAUSE) {
        if (!s->configured || (s->state != HDA_STREAM_PREPARED && s->state != HDA_STREAM_PAUSED))
            return -EINVAL;
        control |= HDA_SDCTL_RUN | HDA_SDCTL_IOCE | HDA_SDCTL_FEIE | HDA_SDCTL_DEIE;
        s->position_wallclock = hda_mmio_read32(c, HDA_REG_WALLCLK);
        hda_mmio_write32(c, reg, control);
        if (!(hda_mmio_read32(c, reg) & HDA_SDCTL_RUN)) return -EIO;
        uint32_t intctl = hda_mmio_read32(c, HDA_REG_INTCTL_REG);
        hda_mmio_write32(c, HDA_REG_INTCTL_REG, intctl | (1u << s->index));
        if (c->api && c->api->ticks) s->position_tick = c->api->ticks();
        s->state = HDA_STREAM_RUNNING;
        return 0;
    }
    if (trigger == AUDIO_PAUSE) {
        if (s->state != HDA_STREAM_RUNNING) return -EINVAL;
        hda_mmio_write32(c, reg, control & ~HDA_SDCTL_RUN);
        if (hda_mmio_read32(c, reg) & HDA_SDCTL_RUN) return -EIO;
        s->state = HDA_STREAM_PAUSED;
        return 0;
    }
    if (trigger != AUDIO_STOP) return -EINVAL;
    hda_mmio_write32(c, reg, control & ~HDA_SDCTL_RUN);
    if (hda_mmio_read32(c, reg) & HDA_SDCTL_RUN) return -EIO;
    hda_mmio_write32(c, HDA_REG_INTCTL_REG,
                     hda_mmio_read32(c, HDA_REG_INTCTL_REG) & ~(1u << s->index));
    if (s->state != HDA_STREAM_FREE && s->state != HDA_STREAM_STOPPED) {
        s->state = HDA_STREAM_STOPPED;
        (void)hda_controller_stream_release(c);
    }
    return 0;
}

/** @brief Read a monotonic frame count and detect unprovable wraps.
 * @param opaque Controller context.
 * @param stream Borrowed stream lease.
 * @param frames Receives completed frames.
 * @return 0 or -EPIPE for XRUN/unprovable position. IRQ-safe, no wait.
 */
int hda_pcm_pointer(void *opaque, struct audio_hw_stream *stream,
                    uint64_t *frames)
{
    struct hda_controller *c = opaque;
    struct hda_stream *s = hda_stream_from_hw(stream);
    if (!c || !s || !frames || !s->configured || !s->selected.pcm.frame_bytes) return -EINVAL;
    if (s->state == HDA_STREAM_XRUN) return -EPIPE;
    return hda_stream_position_update(s, frames);
}

/** @brief Close a stopped lease while retaining persistent descriptor DMA.
 * @param opaque Controller context.
 * @param stream Borrowed lease consumed by close.
 * @return None; no wait, allocation, or codec operation.
 */
void hda_pcm_close(void *opaque, struct audio_hw_stream *stream)
{
    struct hda_controller *c = opaque;
    struct hda_stream *s = hda_stream_from_hw(stream);
    if (!c || !s || s->controller != c || s->state == HDA_STREAM_RUNNING ||
        s->state == HDA_STREAM_PAUSED) return;
    if (s->group && (s->configured || hda_stream_group_needs_release(s->group)) &&
        hda_route_group_stream_bind(s->group, 0u, 0u)) return;
    if (s->tag) c->stream_tag_mask &= (uint16_t)~(1u << s->tag);
    s->tag = 0u;
    if (s->group && s->group->members) {
        for (uint32_t i = 0; i < s->group->member_count; ++i) {
            s->group->members[i].stream_bound = 0u;
            s->group->members[i].stream_tag = 0u;
            s->group->members[i].stream_format = 0u;
        }
    }
    s->group = NULL;
    s->id = 0u;
    s->configured = 0u;
    s->state = HDA_STREAM_FREE;
    stream->id = 0u;
    stream->driver = NULL;
}

/** @brief Publish bounded stream IRQ completions and W1C only observed status.
 * @param c Initialized controller while its short state lock is held.
 * @param budget Remaining global completion budget.
 * @return Number of period/error notifications consumed, no greater than budget.
 * IRQ-safe, allocation-free, and never waits for codec or core locks.
 * Position and descriptor/FIFO errors clear RUN before XRUN notification;
 * failed STOP retains the running lease and backing for a later retry.
 */
uint32_t hda_stream_service_locked(struct hda_controller *c, uint32_t budget)
{
    if (!c || !c->streams || !budget) return 0u;
    uint32_t status = hda_mmio_read32(c, HDA_REG_INTSTS_REG);
    uint32_t consumed = 0u;
    for (uint32_t i = 0; i < c->stream_count && consumed < budget; ++i) {
        struct hda_stream *s = &c->streams[i];
        if (!(status & (1u << i)) || s->state != HDA_STREAM_RUNNING) continue;
        uint32_t sd_status = hda_mmio_read8(c, hda_stream_offset(s, HDA_SDSTS));
        uint32_t observed = sd_status & (HDA_SDSTS_BCIS | HDA_SDSTS_FIFOE | HDA_SDSTS_DESE);
        if (observed) hda_mmio_write8(c, hda_stream_offset(s, HDA_SDSTS), (uint8_t)observed);
        if (observed & (HDA_SDSTS_FIFOE | HDA_SDSTS_DESE)) {
            uint32_t control = hda_mmio_read32(c, hda_stream_offset(s, HDA_SDCTL));
            hda_mmio_write32(c, hda_stream_offset(s, HDA_SDCTL), control & ~HDA_SDCTL_RUN);
            if (hda_mmio_read32(c, hda_stream_offset(s, HDA_SDCTL)) & HDA_SDCTL_RUN)
                continue;
            s->state = HDA_STREAM_XRUN;
            if (!s->xrun_reported) {
                s->xrun_reported = 1u;
                hda_audio_period_elapsed(s->card_id, s->id, s->frame_position, -EPIPE);
            }
            ++consumed;
            continue;
        }
        if (!(observed & HDA_SDSTS_BCIS)) continue;
        uint64_t completed = 0u;
        if (hda_stream_position_update(s, &completed)) {
            uint32_t control = hda_mmio_read32(c, hda_stream_offset(s, HDA_SDCTL));
            hda_mmio_write32(c, hda_stream_offset(s, HDA_SDCTL), control & ~HDA_SDCTL_RUN);
            if (hda_mmio_read32(c, hda_stream_offset(s, HDA_SDCTL)) & HDA_SDCTL_RUN)
                continue;
            s->state = HDA_STREAM_XRUN;
            if (!s->xrun_reported) {
                s->xrun_reported = 1u;
                hda_audio_period_elapsed(s->card_id, s->id, s->frame_position, -EPIPE);
            }
            ++consumed;
            continue;
        }
        /* A pointer query may already have observed this IRQ's position.
         * A repeated status does not invent progress or force an XRUN. */
        if (completed == s->reported_frames) continue;
        s->reported_frames = completed;
        s->last_period = (uint32_t)(completed / s->selected.pcm.period_frames) % s->period_count;
        hda_audio_period_elapsed(s->card_id, s->id, completed, 0);
        ++consumed;
    }
    return consumed;
}

/** @brief Bind an accepted H3 group without changing route snapshots.
 * @param stream Open stream lease.
 * @param group H3 group retained by the caller through stream close.
 * @return 0 or a validation/state errno. Task context only.
 */
int hda_stream_bind_group(struct hda_stream *stream,
                          struct hda_route_group *group)
{
    if (!stream || !group || stream->state != HDA_STREAM_OPEN ||
        !group->members || !group->member_count || !group->total_channels ||
        group->total_channels > 16u || !group->active) return -EINVAL;
    for (uint32_t i = 0; i < group->member_count; ++i) {
        const struct hda_route *route = &group->members[i];
        if (!route->channel_count || route->channel_start + route->channel_count > 16u)
            return -ERANGE;
    }
    stream->group = group;
    return 0;
}

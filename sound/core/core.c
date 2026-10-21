#include <stddef.h>
#include <sound/asound.h>
#include <linux/errno.h>
#include <reliefnt/audio.h>
#include <reliefnt/lock.h>
#include <reliefnt/sched.h>
#include <reliefnt/wait.h>
#define CARD_MAX 16U
#define STREAM_MAX 64U
#define SLOT_MASK 255U
#define AUDIO_CONTROL_MAX 64U
struct card_entry {
    uint32_t id, owner, live, closing, calls, streams, servicing, service_changing;
    uint32_t disconnect_pending, disconnecting;
    uint32_t task_pending, task_servicing;
    struct audio_card_identity identity;
    struct audio_card_ops ops;
    void *opaque;
    audio_service_fn service;
    void *service_opaque;
    uint64_t periods, frames, controls;
    uint32_t control_count;
    int error;
};
struct stream_entry {
    uint32_t id, card, device, live, detached, calls;
    enum audio_direction direction;
    struct audio_hw_stream hw;
    uint64_t period_frames;
    int period_error;
    uint32_t period_seq, period_seen;
};
static struct card_entry cards[CARD_MAX];
static struct stream_entry streams[STREAM_MAX];
static uint32_t card_generation, stream_generation, service_cursor, task_service_cursor;
static audio_pcm_notify_fn audio_pcm_notifier;
static struct kernel_spinlock audio_lock = KERNEL_SPINLOCK_INIT;
/* Control events may originate in IRQ context.  The queue is statically
 * initialized so no allocation or task-context setup is needed on the first
 * notification. */
static struct kernel_wait_queue audio_control_waiters = {
    .lock = KERNEL_SPINLOCK_INIT,
};
static uint32_t audio_control_epoch;
static struct kernel_wait_queue audio_pcm_waiters = {
    .lock = KERNEL_SPINLOCK_INIT,
};
static uint32_t audio_pcm_epoch;
static struct audio_control_event_queue *audio_control_queues[AUDIO_CONTROL_EVENT_SLOTS];
/** @brief Look up a generation-tagged card while registry lock is held.
 * @param id Opaque generation and slot token; borrowed, no ownership transfer.
 * @return Live or disconnecting entry, or NULL. IRQ safe, no sleep.
 */
static struct card_entry *find_card(uint32_t id)
{
    uint32_t slot = id & SLOT_MASK;
    return slot < CARD_MAX && cards[slot].id == id && cards[slot].live ? &cards[slot] : NULL;
}
/** @brief Look up an open stream under registry lock.
 * @param id Opaque generation and slot token.
 * @return Owned core entry, or NULL. IRQ safe, no sleep.
 */
static struct stream_entry *find_stream(uint32_t id)
{
    uint32_t slot = id & SLOT_MASK;
    return slot < STREAM_MAX && streams[slot].id == id && streams[slot].live ? &streams[slot] : NULL;
}
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
                             uint32_t owner, uint32_t *out_id)
{
    const uint32_t v1_end = (uint32_t)(offsetof(struct audio_card_ops, control_write) +
                                       sizeof(ops->control_write));
    const uint32_t caps_end = (uint32_t)(offsetof(struct audio_card_ops, pcm_format_caps) +
                                          sizeof(ops->pcm_format_caps));
    const uint32_t prepare_format_end = (uint32_t)(offsetof(struct audio_card_ops, prepare_format) +
                                                   sizeof(ops->prepare_format));
    const uint32_t task_service_end = (uint32_t)(offsetof(struct audio_card_ops, task_service) +
                                                  sizeof(ops->task_service));
    if (!identity || !ops || !out_id || ops->version != AUDIO_CARD_OPS_VERSION ||
        ops->size < v1_end || !ops->pcm_caps || !ops->open || !ops->prepare ||
        !ops->trigger || !ops->pointer || !ops->close || !ops->control_count ||
        !identity->id[0] || !identity->name[0]) return -22;
    uint32_t registered_controls = ops->control_count(opaque);
    if (registered_controls > AUDIO_CONTROL_MAX) registered_controls = AUDIO_CONTROL_MAX;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    for (uint32_t i = 0; i < CARD_MAX; ++i) {
        if (cards[i].live) continue;
        /* Never wrap and reuse a generation token. */
        if (card_generation == 0xffffffu) break;
        cards[i] = (struct card_entry){0};
        cards[i].id = (++card_generation << 8) | i;
        cards[i].owner = owner;
        cards[i].live = 1;
        cards[i].identity = *identity;
        cards[i].ops.version = ops->version;
        cards[i].ops.size = ops->size < sizeof(cards[i].ops) ? ops->size : sizeof(cards[i].ops);
        cards[i].ops.pcm_caps = ops->pcm_caps;
        cards[i].ops.open = ops->open;
        cards[i].ops.prepare = ops->prepare;
        cards[i].ops.trigger = ops->trigger;
        cards[i].ops.pointer = ops->pointer;
        cards[i].ops.close = ops->close;
        cards[i].ops.control_count = ops->control_count;
        if (ops->size >= offsetof(struct audio_card_ops, control_info) + sizeof(ops->control_info))
            cards[i].ops.control_info = ops->control_info;
        if (ops->size >= offsetof(struct audio_card_ops, control_read) + sizeof(ops->control_read))
            cards[i].ops.control_read = ops->control_read;
        if (ops->size >= v1_end) cards[i].ops.control_write = ops->control_write;
        if (ops->size >= caps_end) cards[i].ops.pcm_format_caps = ops->pcm_format_caps;
        if (ops->size >= prepare_format_end) cards[i].ops.prepare_format = ops->prepare_format;
        if (ops->size >= task_service_end) cards[i].ops.task_service = ops->task_service;
        cards[i].opaque = opaque;
        cards[i].control_count = registered_controls;
        cards[i].identity.id[15] = 0;
        cards[i].identity.name[79] = 0;
        *out_id = cards[i].id;
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
        return 0;
    }
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return -28;
}
/** @brief Release a callback/module execution pin under the registry lock.
 * @param c Borrowed pinned card.
 * @param s Optional pinned stream.
 * @return None. IRQ safe; never waits or calls a module.
 */
static void call_done(struct card_entry *c, struct stream_entry *s)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    __atomic_sub_fetch(&c->calls, 1, __ATOMIC_RELEASE);
    if (s) __atomic_sub_fetch(&s->calls, 1, __ATOMIC_RELEASE);
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
}
/** @brief Wait for callback pins to drain without holding any registry lock.
 * @param calls Atomic count whose owner remains reserved until this returns.
 * @return None. Task context only; no execution lock; never called by a callback.
 */
static void synchronize_calls(uint32_t *calls)
{
    while (__atomic_load_n(calls, __ATOMIC_ACQUIRE)) __asm__ volatile("pause" ::: "memory");
}
/** @brief Set the card service, synchronizing replaced callbacks.
 * @param id Generation card token.
 * @param callback Nonblocking service, or NULL to remove.
 * @param opaque Borrowed context held alive until replacement/removal returns.
 * @return 0, -ENODEV or -EBUSY. Task context, no execution lock/callback nesting.
 */
int audio_card_set_service(uint32_t id, audio_service_fn callback, void *opaque)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(id);
    if (!c || c->closing) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -19; }
    /* Replacement is deliberately explicit: remove, synchronize, then install. */
    if (c->service_changing || (c->service && callback)) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -16; }
    c->service_changing = 1;
    c->service = callback; c->service_opaque = opaque;
    __atomic_add_fetch(&c->calls, 1, __ATOMIC_ACQUIRE);
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    if (!callback) while (__atomic_load_n(&c->calls, __ATOMIC_ACQUIRE) > 1) __asm__ volatile("pause" ::: "memory");
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    c->service_changing = 0;
    __atomic_sub_fetch(&c->calls, 1, __ATOMIC_RELEASE);
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return 0;
}
/** @brief Run one globally bounded tick, rotating the starting card for fairness.
 * @return None. IRQ context; each callback is pinned and invoked without lock.
 * Callbacks consume at most a total of 32 completions and must never sleep.
 * Optional task callbacks are coalesced for a later task-context pump.
 */
void audio_service_tick(void)
{
    uint32_t budget = 32;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    uint32_t start = service_cursor++ % CARD_MAX;
    for (uint32_t i = 0; i < CARD_MAX; ++i)
        if (cards[i].live && !cards[i].closing && cards[i].ops.task_service)
            cards[i].task_pending = 1;
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    for (uint32_t n = 0; n < CARD_MAX && budget; ++n) {
        kernel_spin_lock_irqsave(&audio_lock, &flags);
        struct card_entry *c = &cards[(start + n) % CARD_MAX];
        audio_service_fn fn = c->live && !c->closing && !c->servicing && !c->service_changing ? c->service : NULL;
        void *opaque = c->service_opaque;
        if (fn) { c->servicing = 1; __atomic_add_fetch(&c->calls, 1, __ATOMIC_ACQUIRE); }
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
        if (!fn) continue;
        uint32_t used = fn(opaque, budget);
        budget -= used > budget ? budget : used;
        kernel_spin_lock_irqsave(&audio_lock, &flags);
        c->servicing = 0;
        __atomic_sub_fetch(&c->calls, 1, __ATOMIC_RELEASE);
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
    }
}
/** @brief Inspect coalesced work for live cards with task callbacks.
 * @return True when a card needs task-context service. IRQ safe; no callbacks.
 */
bool audio_task_work_pending(void)
{
    uint64_t flags;
    bool pending = false;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    for (uint32_t i = 0; i < CARD_MAX; ++i)
        if (cards[i].live && !cards[i].closing && cards[i].task_pending &&
            cards[i].ops.task_service) { pending = true; break; }
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return pending;
}
/** @brief Run bounded, pinned task callbacks, visiting each card at most once.
 * @param budget Global work limit; zero preserves all pending work.
 * @return Work consumed, no greater than budget. Caller owns manager lifecycle
 * admission and has released execution ownership with IRQs enabled. No core
 * lock spans a callback; IRQ service can progress while task callbacks wait.
 */
uint32_t audio_process_task_work(uint32_t budget)
{
    if (!budget) return 0;
    uint32_t remaining = budget;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    uint32_t start = task_service_cursor % CARD_MAX;
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    for (uint32_t n = 0; n < CARD_MAX && remaining; ++n) {
        uint32_t slot = (start + n) % CARD_MAX;
        kernel_spin_lock_irqsave(&audio_lock, &flags);
        struct card_entry *c = &cards[slot];
        uint32_t (*fn)(void *, uint32_t) =
            c->live && !c->closing && c->task_pending && !c->task_servicing ?
                c->ops.task_service : NULL;
        void *opaque = c->opaque;
        task_service_cursor = (slot + 1) % CARD_MAX;
        if (fn) {
            /* Clear before dispatch so a concurrent tick retains new work. */
            c->task_pending = 0;
            c->task_servicing = 1;
            __atomic_add_fetch(&c->calls, 1, __ATOMIC_ACQUIRE);
        }
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
        if (!fn) continue;
        uint32_t used = fn(opaque, remaining);
        remaining -= used > remaining ? remaining : used;
        kernel_spin_lock_irqsave(&audio_lock, &flags);
        c->task_servicing = 0;
        __atomic_sub_fetch(&c->calls, 1, __ATOMIC_RELEASE);
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
    }
    return budget - remaining;
}
/** @brief Open a core-owned stream lease and serialize its eventual disconnect.
 * @param card Generation token for card.
 * @param device PCM device index.
 * @param direction Valid playback/capture selector.
 * @param out_id Receives stream generation token.
 * @return 0 or negative errno. Task context without execution lock; callback may wait.
 */
int audio_stream_open(uint32_t card, uint32_t device, enum audio_direction direction, uint32_t *out_id)
{
    if (!out_id || (uint32_t)direction > AUDIO_CAPTURE) return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(card);
    if (!c || c->closing) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -19; }
    struct stream_entry *s = NULL;
    for (uint32_t i = 0; i < STREAM_MAX; ++i) if (!streams[i].live) { s = &streams[i]; break; }
    if (!s || stream_generation == 0xffffffu) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -28; }
    *s = (struct stream_entry){.id = (++stream_generation << 8) | (uint32_t)(s - streams),
        .card = card, .device = device, .direction = direction, .live = 1, .calls = 1};
    __atomic_add_fetch(&c->calls, 1, __ATOMIC_ACQUIRE); ++c->streams;
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    int ret = c->ops.open(c->opaque, device, direction, &s->hw);
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    __atomic_sub_fetch(&c->calls, 1, __ATOMIC_RELEASE); __atomic_sub_fetch(&s->calls, 1, __ATOMIC_RELEASE);
    if (ret) { s->live = 0; --c->streams; }
    else *out_id = s->id;
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return ret;
}
/** @brief Query the basic PCM constraints through a pinned card callback. */
int audio_card_pcm_caps(uint32_t card, uint32_t device, enum audio_direction direction,
                        struct audio_caps *caps)
{
    if (!caps || (uint32_t)direction > AUDIO_CAPTURE) return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(card);
    if (!c || c->closing) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -19; }
    __atomic_add_fetch(&c->calls, 1, __ATOMIC_ACQUIRE);
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    int ret = c->ops.pcm_caps(c->opaque, device, direction, caps);
    call_done(c, NULL);
    return ret;
}
/** @brief Pin an attached stream callback while registry lock is held.
 * @param id Core stream generation token.
 * @param out Receives associated pinned card.
 * @return Pinned stream or NULL. IRQ safe and nonblocking.
 */
static struct stream_entry *stream_call(uint32_t id, struct card_entry **out)
{
    struct stream_entry *s = find_stream(id);
    if (!s || s->detached || s->calls) return NULL;
    struct card_entry *c = find_card(s->card);
    if (!c || c->closing) return NULL;
    __atomic_add_fetch(&c->calls, 1, __ATOMIC_ACQUIRE); __atomic_add_fetch(&s->calls, 1, __ATOMIC_ACQUIRE); *out = c;
    return s;
}
/** @brief Dispatch prepare through a pinned module callback.
 * @param id Core stream token.
 * @param params Borrowed validated hardware parameters.
 * @param dma Borrowed core-owned pinned buffer; not freed by this function.
 * @return Driver status or -ENODEV/-EINVAL. Task context without execution lock.
 */
int audio_stream_prepare(uint32_t id, const struct audio_params *params, const struct audio_dma *dma)
{
    if (!params || !dma) return -22;
    uint64_t flags; struct card_entry *c = NULL;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct stream_entry *s = stream_call(id, &c);
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    if (!s) return -19;
    int ret = c->ops.prepare(c->opaque, &s->hw, params, dma); call_done(c, s); return ret;
}
/** @brief Query selected-format capabilities through a pinned card callback.
 * @param card Generation card token.
 * @param device PCM device index.
 * @param direction Playback/capture selector.
 * @param caps Receives bounded selected-format metadata.
 * @return 0, -ENODEV, -EOPNOTSUPP, or callback errno. Task context.
 */
int audio_stream_format_caps(uint32_t card, uint32_t device,
                             enum audio_direction direction,
                             struct audio_format_caps *caps)
{
    if (!caps || (uint32_t)direction > AUDIO_CAPTURE) return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(card);
    if (!c || c->closing) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -19; }
    if (!c->ops.pcm_format_caps) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -95; }
    __atomic_add_fetch(&c->calls, 1, __ATOMIC_ACQUIRE);
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    int ret = c->ops.pcm_format_caps(c->opaque, device, direction, caps);
    call_done(c, NULL);
    return ret;
}
/** @brief Dispatch explicit selected-format prepare through a pinned callback.
 * @param id Core stream token.
 * @param params Borrowed selected memory format/subformat metadata.
 * @param dma Borrowed core-owned pinned DMA buffer.
 * @return 0, -ENODEV, -EOPNOTSUPP, or callback errno. Task context.
 */
int audio_stream_prepare_format(uint32_t id, const struct audio_params_ext *params,
                                const struct audio_dma *dma)
{
    if (!params || !dma) return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = NULL;
    struct stream_entry *s = stream_call(id, &c);
    if (!s) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -19; }
    if (!c->ops.prepare_format) {
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
        call_done(c, s);
        return -95;
    }
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    int ret = c->ops.prepare_format(c->opaque, &s->hw, params, dma);
    call_done(c, s);
    return ret;
}
/** @brief Trigger an attached hardware stream using a module execution pin.
 * @param id Core stream token.
 * @param trigger START/STOP/PAUSE/UNPAUSE; STOP synchronously quiesces DMA.
 * @return Driver status or -EINVAL/-ENODEV. IRQ safe; callback cannot sleep.
 */
int audio_stream_trigger(uint32_t id, enum audio_trigger trigger)
{
    if ((uint32_t)trigger > AUDIO_UNPAUSE) return -22;
    uint64_t flags; struct card_entry *c = NULL;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct stream_entry *s = stream_call(id, &c);
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    if (!s) return -19;
    int ret = c->ops.trigger(c->opaque, &s->hw, trigger); call_done(c, s); return ret;
}
/** @brief Read monotonic hardware position through a pinned callback.
 * @param id Core stream token.
 * @param frames Receives hardware frames.
 * @return Driver status or -ENODEV/-EINVAL. IRQ safe; never sleeps.
 */
int audio_stream_pointer(uint32_t id, uint64_t *frames)
{
    if (!frames) return -22;
    uint64_t flags; struct card_entry *c = NULL;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct stream_entry *s = stream_call(id, &c);
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    if (!s) return -19;
    int ret = c->ops.pointer(c->opaque, &s->hw, frames); call_done(c, s); return ret;
}
/** @brief Consume one coalesced period notification without calling module code. */
int audio_stream_period_poll(uint32_t id, uint64_t *frames, int *error)
{
    if (!frames || !error) return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct stream_entry *s = find_stream(id);
    if (!s || s->detached) {
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
        return -19;
    }
    if (s->period_seq == s->period_seen) {
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
        return 0;
    }
    *frames = s->period_frames;
    *error = s->period_error;
    s->period_seen = s->period_seq;
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return 1;
}
/** @brief Close a stream lease after the last fd/VMA owner drops it.
 * @param id Core-owned stream token. Caller serializes last-reference close.
 * @return 0, queued disconnect errno or STOP error; failure retains the lease
 * and module. Task context
 * without execution lock; detached leases call no module.
 * DMA page release belongs to the PCM owner, after this returns.
 */
int audio_stream_close(uint32_t id)
{
    uint64_t flags;
    for (;;) {
        kernel_spin_lock_irqsave(&audio_lock, &flags);
        struct stream_entry *s = find_stream(id);
        if (!s) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return 0; }
        struct card_entry *c = find_card(s->card);
        if (s->detached || !c) {
            if (s->calls) { kernel_spin_unlock_irqrestore(&audio_lock, flags); continue; }
            s->live = 0; kernel_spin_unlock_irqrestore(&audio_lock, flags); return 0;
        }
        if (c->closing) {
            if(c->disconnect_pending && !c->disconnecting){
                int ret=c->error ? c->error : -ENODEV;
                kernel_spin_unlock_irqrestore(&audio_lock,flags);return ret;
            }
            kernel_spin_unlock_irqrestore(&audio_lock, flags); continue;
        }
        __atomic_add_fetch(&c->calls, 1, __ATOMIC_ACQUIRE);
        s->detached = 1;
        __atomic_add_fetch(&s->calls, 1, __ATOMIC_ACQUIRE);
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
        while (__atomic_load_n(&s->calls,__ATOMIC_ACQUIRE)>1) __asm__ volatile("pause" ::: "memory");
        int ret = c->ops.trigger(c->opaque, &s->hw, AUDIO_STOP);
        if (ret) {
            kernel_spin_lock_irqsave(&audio_lock,&flags);
            s->detached=0;
            __atomic_sub_fetch(&s->calls,1,__ATOMIC_RELEASE);
            __atomic_sub_fetch(&c->calls,1,__ATOMIC_RELEASE);
            kernel_spin_unlock_irqrestore(&audio_lock,flags);
            return ret;
        }
        c->ops.close(c->opaque, &s->hw);
        kernel_spin_lock_irqsave(&audio_lock, &flags);
        --c->streams; __atomic_sub_fetch(&c->calls, 1, __ATOMIC_RELEASE);
        __atomic_sub_fetch(&s->calls, 1, __ATOMIC_RELEASE); s->live = 0;
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
        return 0;
    }
}
/** @brief Quiesce a card reserved as closing, preserving leases on STOP failure.
 * @param c Borrowed reserved entry whose module owner remains mapped.
 * @return 0 or STOP error. Task context; drains callbacks without registry lock.
 * Closing blocks new service/stream callbacks; detached stream objects stay live
 * for fd/VMA owners. A fatal request keeps admission closed on STOP failure;
 * ordinary unload failures reopen the card for recovery.
 */
static int disconnect_card(struct card_entry *c)
{
    uint64_t flags,frames[STREAM_MAX];uint32_t tokens[STREAM_MAX],count=0,id=c->id;
    synchronize_calls(&c->calls);
    for (uint32_t i=0;i<STREAM_MAX;++i) {
        kernel_spin_lock_irqsave(&audio_lock,&flags);
        struct stream_entry *s=&streams[i];
        bool stop=s->live && s->card==id && !s->detached;
        if(stop){s->detached=1;__atomic_add_fetch(&s->calls,1,__ATOMIC_ACQUIRE);}
        kernel_spin_unlock_irqrestore(&audio_lock,flags);
        if(!stop)continue;
        int ret=c->ops.trigger(c->opaque,&s->hw,AUDIO_STOP);
        if(!ret)c->ops.close(c->opaque,&s->hw);
        kernel_spin_lock_irqsave(&audio_lock,&flags);
        __atomic_sub_fetch(&s->calls,1,__ATOMIC_RELEASE);
        if(ret){s->detached=0;c->closing=c->disconnect_pending;c->disconnecting=0;
            c->error=ret;kernel_spin_unlock_irqrestore(&audio_lock,flags);return ret;}
        --c->streams;
        kernel_spin_unlock_irqrestore(&audio_lock,flags);
    }
    kernel_spin_lock_irqsave(&audio_lock,&flags);
    c->live=0;c->service=NULL;c->opaque=NULL;c->ops=(struct audio_card_ops){0};
    c->disconnect_pending=0;c->disconnecting=0;
    ++audio_control_epoch;
    ++audio_pcm_epoch;
    audio_pcm_notify_fn notify=audio_pcm_notifier;
    for(uint32_t i=0;i<STREAM_MAX;++i)
        if(streams[i].live && streams[i].card==id){
            tokens[count]=streams[i].id;frames[count++]=streams[i].period_frames;
        }
    kernel_spin_unlock_irqrestore(&audio_lock,flags);
    (void)kernel_wait_queue_wake_all(&audio_control_waiters);
    if(notify)for(uint32_t i=0;i<count;++i)notify(tokens[i],frames[i],-ENODEV);
    (void)kernel_wait_queue_wake_all(&audio_pcm_waiters);
    return 0;
}
/** @brief Disconnect a card before its module is finalized.
 * @param id Card generation token.
 * @param force Nonzero permits disconnect of open stream leases.
 * @return 0, -ENODEV, -EBUSY or STOP error (module must remain mapped).
 * Task context, never from callbacks/IRQ. Closing cancels service admission,
 * synchronizes callbacks and stops/closes hardware while preserving fd/VMA leases.
 */
int audio_unregister_card(uint32_t id,uint32_t force)
{
    uint64_t flags;kernel_spin_lock_irqsave(&audio_lock,&flags);
    struct card_entry *c=find_card(id);
    if(!c){kernel_spin_unlock_irqrestore(&audio_lock,flags);return -19;}
    if(c->disconnecting || (c->closing && !(force && c->disconnect_pending)) ||
        (!force && (c->streams || c->calls))){kernel_spin_unlock_irqrestore(&audio_lock,flags);return -16;}
    c->closing=1;c->disconnecting=1;
    kernel_spin_unlock_irqrestore(&audio_lock,flags);return disconnect_card(c);
}
/** @brief Reserve all a module's cards against new opens before disconnecting.
 * @param owner Driver manager slot; memory must stay alive until return.
 * @param force Nonzero forces hardware disconnect while core leases remain.
 * @return 0, -EBUSY or STOP error; failure forbids module image release.
 * Task context; registry lock provides atomic all-card busy preflight/reservation.
 */
int audio_unregister_owner(uint32_t owner,uint32_t force)
{
    uint64_t flags;struct card_entry *owned[CARD_MAX];uint32_t count=0;
    kernel_spin_lock_irqsave(&audio_lock,&flags);
    for(uint32_t i=0;i<CARD_MAX;++i)if(cards[i].live && cards[i].owner==owner){
        if(cards[i].disconnecting ||
            (cards[i].closing && !(force && cards[i].disconnect_pending)) ||
            (!force && (cards[i].streams || cards[i].calls))){kernel_spin_unlock_irqrestore(&audio_lock,flags);return -16;}
        owned[count++]=&cards[i];
    }
    for(uint32_t i=0;i<count;++i){owned[i]->closing=1;owned[i]->disconnecting=1;}
    kernel_spin_unlock_irqrestore(&audio_lock,flags);
    for(uint32_t i=0;i<count;++i){
        int ret=disconnect_card(owned[i]);
        if(ret){
            kernel_spin_lock_irqsave(&audio_lock,&flags);
            for(uint32_t j=i+1;j<count;++j){owned[j]->closing=owned[j]->disconnect_pending;owned[j]->disconnecting=0;}
            kernel_spin_unlock_irqrestore(&audio_lock,flags);return ret;
        }
    }
    return 0;
}
/** @brief Queue fatal disconnect for cards sharing a module owner and PCI BDF.
 * @param card Live generation token identifying the affected controller.
 * @return Zero or -ENODEV. IRQ safe; closes admission and wakes readers with
 * ENODEV without any module callback, DMA stop, wait or resource release.
 */
int audio_request_controller_disconnect(uint32_t card)
{
    uint64_t flags,frames[STREAM_MAX];uint32_t tokens[STREAM_MAX],count=0;
    uint32_t affected=0;
    kernel_spin_lock_irqsave(&audio_lock,&flags);
    struct card_entry *source=find_card(card);
    if(!source){kernel_spin_unlock_irqrestore(&audio_lock,flags);return -ENODEV;}
    for(uint32_t i=0;i<CARD_MAX;++i){
        struct card_entry *c=&cards[i];
        if(!c->live || c->owner!=source->owner ||
            c->identity.bus!=source->identity.bus || c->identity.slot!=source->identity.slot ||
            c->identity.function!=source->identity.function)continue;
        if(c->disconnect_pending)continue;
        c->disconnect_pending=1;c->closing=1;c->error=-ENODEV;
        affected|=1u<<i;
    }
    for(uint32_t i=0;i<STREAM_MAX;++i){
        struct stream_entry *s=&streams[i];
        uint32_t slot=s->card&SLOT_MASK;
        if(!s->live || slot>=CARD_MAX || cards[slot].id!=s->card ||
            !(affected&(1u<<slot)))continue;
        s->period_error=-ENODEV;++s->period_seq;
        tokens[count]=s->id;frames[count++]=s->period_frames;
    }
    if(affected){++audio_control_epoch;++audio_pcm_epoch;}
    audio_pcm_notify_fn notify=audio_pcm_notifier;
    kernel_spin_unlock_irqrestore(&audio_lock,flags);
    if(affected){
        if(notify)for(uint32_t i=0;i<count;++i)notify(tokens[i],frames[i],-ENODEV);
        (void)kernel_wait_queue_wake_all(&audio_control_waiters);
        (void)kernel_wait_queue_wake_all(&audio_pcm_waiters);
    }
    return 0;
}
/** @brief Inspect whether fatal disconnect requests need task-context service.
 * @return True for pending cards, including failed STOP retries. IRQ safe.
 */
bool audio_disconnect_work_pending(void)
{
    uint64_t flags;bool pending=false;
    kernel_spin_lock_irqsave(&audio_lock,&flags);
    for(uint32_t i=0;i<CARD_MAX;++i)
        if(cards[i].live && cards[i].disconnect_pending){pending=true;break;}
    kernel_spin_unlock_irqrestore(&audio_lock,flags);return pending;
}
/** @brief Stop and disconnect a bounded number of pending cards in task context.
 * @param budget Maximum card attempts, including failed STOP attempts.
 * @return Attempt count. Caller owns manager lifecycle admission and has
 * released execution ownership; module images remain mapped throughout.
 * Failed STOP keeps DMA, card metadata and the pending request for retry.
 */
uint32_t audio_process_pending_disconnects(uint32_t budget)
{
    uint64_t flags;uint32_t work=0;
    for(uint32_t i=0;i<CARD_MAX && work<budget;++i){
        kernel_spin_lock_irqsave(&audio_lock,&flags);
        struct card_entry *c=&cards[i];
        bool run=c->live && c->disconnect_pending && !c->disconnecting;
        if(run){c->closing=1;c->disconnecting=1;}
        kernel_spin_unlock_irqrestore(&audio_lock,flags);
        if(!run)continue;
        (void)disconnect_card(c);++work;
    }
    return work;
}
/** @brief Install the permanent built-in PCM metadata notification hook.
 * @param notify Non-sleeping kernel-resident hook, or NULL before PCM use.
 * @return None. Task context; never a module callback. The hook runs after
 * releasing the registry lock and has permanent text/storage lifetime.
 */
void audio_pcm_set_notifier(audio_pcm_notify_fn notify)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock,&flags);
    audio_pcm_notifier = notify;
    kernel_spin_unlock_irqrestore(&audio_lock,flags);
}
/** @brief Submit a bounded period notification; stale generations are ignored.
 * @param card Card token held by driver.
 * @param stream Driver hardware stream id (matched by later PCM subscriber).
 * @param frames Completed frame count.
 * @param error Negative stream error, or zero.
 * @return None. IRQ safe; only updates core counters, never calls module/userland.
 */
void audio_period_elapsed(uint32_t card, uint32_t stream, uint64_t frames, int error)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(card);
    struct stream_entry *matched_stream = NULL;
    if (c && !c->closing && stream) {
        for (uint32_t i = 0; i < STREAM_MAX; ++i) {
            if (streams[i].live && !streams[i].detached &&
                streams[i].card == card && streams[i].hw.id == stream) {
                matched_stream = &streams[i];
                break;
            }
        }
    }
    if (matched_stream) {
        matched_stream->period_frames = frames;
        matched_stream->period_error = error;
        ++matched_stream->period_seq;
        ++audio_pcm_epoch;
        ++c->periods; c->frames += frames; c->error = error;
    }
    audio_pcm_notify_fn notify = audio_pcm_notifier;
    uint32_t token = matched_stream ? matched_stream->id : 0;
    uint32_t timer_card = matched_stream ? matched_stream->card : 0;
    uint32_t timer_device = matched_stream ? matched_stream->device : 0;
    enum audio_direction timer_direction = matched_stream
        ? matched_stream->direction : AUDIO_PLAYBACK;
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    if (token && notify) notify(token,frames,error);
    if (token) audio_timer_period_elapsed(timer_card, timer_device,
                                          timer_direction, frames, error);
    if (token) (void)kernel_wait_queue_wake_all(&audio_pcm_waiters);
}
/** @brief Snapshot the IRQ PCM notification epoch before probing a transfer.
 * @return Wrapping epoch; task/IRQ safe. No reference or ownership transfer.
 */
uint32_t audio_pcm_event_epoch(void)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    uint32_t epoch = audio_pcm_epoch;
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return epoch;
}

/** @brief Register the current PCM caller and publish interruptible sleep.
 * @return None. Task context under execution ownership; the pinned OFD remains
 * held across retry. Recheck the event epoch after publishing sleep. A full
 * wait queue leaves the caller runnable for the scheduler retry fallback.
 */
void audio_pcm_wait_current(void)
{
    struct task *task = sched_current_task();
    if (!task || !task->pid || task->state == TASK_EXITED) return;
    if (kernel_wait_queue_add(&audio_pcm_waiters, task) < 0) return;
    sched_signal_wait_current(0);
}

/** @brief Remove a completed or stale PCM wait on syscall entry.
 * @param task Borrowed task pinned by the execution transaction.
 * @return None. Task context; preserves scheduler state and OFD ownership.
 */
void audio_pcm_wait_remove(struct task *task)
{
    kernel_wait_queue_remove(&audio_pcm_waiters, task);
}
/** @brief Record a masked notification and wake registered control readers.
 * @param card Live generation token; stale tokens are ignored.
 * @param control Zero-based real control index.
 * @param mask ALSA event bits to merge into each subscribed OFD's pending item.
 * @return None. IRQ safe; no allocation or module callback.
 */
void audio_control_changed_mask(uint32_t card, uint32_t control, uint32_t mask)
{
    uint64_t flags;
    bool notify = false;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(card);
    if (c && !c->closing) {
        ++c->controls;
        if (control < c->control_count) {
            uint32_t event_mask = mask ? mask : 1u;
            for (uint32_t i = 0; i < AUDIO_CONTROL_EVENT_SLOTS; ++i) {
                struct audio_control_event_queue *queue = audio_control_queues[i];
                if (!queue || queue->card != card || !queue->subscribed) continue;
                if (!queue->pending[control]) {
                    /* Each fixed element occupies at most one pending slot,
                     * so this insertion cannot exceed the element capacity. */
                    queue->order[(queue->head + queue->length) % AUDIO_CONTROL_EVENT_SLOTS] = control;
                    ++queue->length;
                }
                queue->pending[control] |= event_mask;
            }
            ++audio_control_epoch;
            notify = true;
        }
    }
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    if (notify) (void)kernel_wait_queue_wake_all(&audio_control_waiters);
}
/** @brief Record a VALUE event and wake registered control readers.
 * @param card Live generation token; stale tokens are ignored.
 * @param control Zero-based real control index.
 * @return None. IRQ safe; no allocation or module callback.
 */
void audio_control_changed(uint32_t card, uint32_t control)
{
    audio_control_changed_mask(card, control, 1u);
}

/** @brief Snapshot the control notification/disconnect epoch under the registry lock.
 * @return Wrapping event epoch; compare before probing and after publishing sleep.
 */
uint32_t audio_control_event_epoch(void)
{
    uint64_t flags;
    uint32_t epoch;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    epoch = audio_control_epoch;
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return epoch;
}

/** @brief Register the current task and publish an interruptible control sleep.
 * @return None. Task context; caller must recheck the epoch after publication.
 * Queue exhaustion leaves the task runnable for the syscall retry fallback.
 */
void audio_control_wait_current(void)
{
    struct task *task = sched_current_task();
    if (!task || !task->pid || task->state == TASK_EXITED) return;
    if (kernel_wait_queue_add(&audio_control_waiters, task) < 0) return;
    sched_signal_wait_current(0);
}

/** @brief Remove a stale or completed control wait registration.
 * @param task Borrowed task pinned by the caller's execution transaction.
 * @return None. Does not change scheduler state; may be called on syscall retry.
 */
void audio_control_wait_remove(struct task *task)
{
    kernel_wait_queue_remove(&audio_control_waiters, task);
}

/** @brief Copy the identity of a live card through a registry execution pin.
 * @param card Generation token.
 * @param identity Writable identity output.
 * @return Zero, -EINVAL, or -ENODEV. Task context.
 */
int audio_card_identity(uint32_t card, struct audio_card_identity *identity)
{
    if (!identity) return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(card);
    if (!c || c->closing) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -19; }
    *identity = c->identity;
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return 0;
}

/** @brief Report one real PCM device using Linux INFO layout.
 * @param card Live generation token; registry ordinal is returned in INFO.
 * @param info In/out kernel-accessible storage with device/subdevice/stream selectors.
 * @return Zero, -ENXIO for absent device/subdevice/direction, or device errno.
 * Task context; module callbacks run pinned without the registry lock. No
 * reference ownership changes; subdevices_avail reflects active core leases.
 */
int audio_card_pcm_info(uint32_t card, struct snd_pcm_info *info)
{
    if (!info) return -EINVAL;
    uint32_t device = info->device;
    int direction = info->stream;
    if (device > 7u || info->subdevice || direction < 0 || direction > AUDIO_CAPTURE)
        return -ENXIO;
    struct audio_caps caps;
    int ret = audio_card_pcm_caps(card,device,(enum audio_direction)direction,&caps);
    if (ret) return ret == -ENODEV ? ret : -ENXIO;
    struct audio_card_identity identity;
    ret = audio_card_identity(card,&identity);
    if (ret) return ret;
    int ordinal = audio_card_index(card);
    if (ordinal < 0) return -ENODEV;
    __builtin_memset(info,0,sizeof(*info));
    info->device = device;
    info->stream = direction;
    info->card = ordinal;
    __builtin_memcpy(info->id,identity.id,sizeof(identity.id));
    __builtin_memcpy(info->name,identity.name,sizeof(info->name)-1u);
    __builtin_memcpy(info->subname,"subdevice #0",sizeof("subdevice #0"));
    info->dev_class = SNDRV_PCM_CLASS_GENERIC;
    info->dev_subclass = SNDRV_PCM_SUBCLASS_GENERIC_MIX;
    info->subdevices_count = info->subdevices_avail = 1;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock,&flags);
    for (uint32_t i=0;i<STREAM_MAX;++i)
        if (streams[i].live && streams[i].card == card &&
            streams[i].device == device && streams[i].direction == (uint32_t)direction)
            info->subdevices_avail = 0;
    kernel_spin_unlock_irqrestore(&audio_lock,flags);
    return 0;
}

/** @brief Read the fixed control count of a live card.
 * @param card Generation token.
 * @param count Writable count output.
 * @return Zero, -EINVAL, or -ENODEV. Task context.
 */
int audio_card_control_count(uint32_t card, uint32_t *count)
{
    if (!count) return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(card);
    if (!c || c->closing) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -19; }
    *count = c->control_count;
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return 0;
}

/** @brief Query one real control through a pinned card callback.
 * @param card Generation token.
 * @param control Zero-based control index.
 * @param info Writable metadata output.
 * @return Zero or a negative Linux errno. Task context.
 */
int audio_card_control_info(uint32_t card, uint32_t control,
                            struct audio_control_info *info)
{
    if (!info) return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(card);
    if (!c || c->closing) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -19; }
    if (!c->ops.control_info || control >= c->control_count) {
        kernel_spin_unlock_irqrestore(&audio_lock, flags); return -95;
    }
    __atomic_add_fetch(&c->calls, 1, __ATOMIC_ACQUIRE);
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    int ret = c->ops.control_info(c->opaque, control, info);
    call_done(c, NULL);
    return ret;
}

/** @brief Read one real control through a pinned card callback.
 * @param card Generation token.
 * @param control Zero-based control index.
 * @param value Writable value output.
 * @return Zero or a negative Linux errno. Task context.
 */
int audio_card_control_read(uint32_t card, uint32_t control,
                            struct audio_control_value *value)
{
    if (!value) return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(card);
    if (!c || c->closing) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -19; }
    if (!c->ops.control_read || control >= c->control_count) {
        kernel_spin_unlock_irqrestore(&audio_lock, flags); return -95;
    }
    __atomic_add_fetch(&c->calls, 1, __ATOMIC_ACQUIRE);
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    int ret = c->ops.control_read(c->opaque, control, value);
    call_done(c, NULL);
    return ret;
}

/** @brief Write one real control through a pinned card callback.
 * @param card Generation token.
 * @param control Zero-based control index.
 * @param value Validated value input.
 * @return Zero or a negative Linux errno. Task context.
 */
int audio_card_control_write(uint32_t card, uint32_t control,
                             const struct audio_control_value *value)
{
    if (!value) return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(card);
    if (!c || c->closing) { kernel_spin_unlock_irqrestore(&audio_lock, flags); return -19; }
    if (!c->ops.control_write || control >= c->control_count) {
        kernel_spin_unlock_irqrestore(&audio_lock, flags); return -95;
    }
    __atomic_add_fetch(&c->calls, 1, __ATOMIC_ACQUIRE);
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    int ret = c->ops.control_write(c->opaque, control, value);
    call_done(c, NULL);
    return ret;
}

/** @brief Test membership while the audio registry lock protects queue lifetime.
 * @param queue Borrowed address; no dereference until membership is established.
 * @return True for registered storage, false otherwise. IRQ-safe, no callback.
 */
static bool audio_control_queue_registered(const struct audio_control_event_queue *queue)
{
    if (!queue) return false;
    for (uint32_t i = 0; i < AUDIO_CONTROL_EVENT_SLOTS; ++i)
        if (audio_control_queues[i] == queue) return true;
    return false;
}

/** @brief Attach preallocated OFD event storage to a live fixed-control card.
 * @param card Live generation token; no module reference is transferred.
 * @param queue Unregistered writable storage retained until queue_close.
 * @return Zero, -EINVAL, -ENODEV, -EBUSY, or -ENFILE; no callback or allocation.
 */
int audio_control_queue_open(uint32_t card, struct audio_control_event_queue *queue)
{
    if (!queue) return -EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    int ret = -ENFILE;
    struct card_entry *c = find_card(card);
    if (!c || c->closing) ret = -ENODEV;
    else if (audio_control_queue_registered(queue)) ret = -EBUSY;
    else {
        for (uint32_t i = 0; i < AUDIO_CONTROL_EVENT_SLOTS; ++i) {
            if (audio_control_queues[i]) continue;
            *queue = (struct audio_control_event_queue){.card = card};
            audio_control_queues[i] = queue;
            ret = 0;
            break;
        }
    }
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return ret;
}

/** @brief Detach event storage before its OFD is released or reused.
 * @param queue Registered storage; remains valid throughout this call.
 * @return Zero or -EBADF; IRQ-safe serialization, also after card disconnect.
 */
int audio_control_queue_close(struct audio_control_event_queue *queue)
{
    uint64_t flags;
    int ret = -EBADF;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    if (queue) {
        for (uint32_t i = 0; i < AUDIO_CONTROL_EVENT_SLOTS; ++i) {
            if (audio_control_queues[i] != queue) continue;
            audio_control_queues[i] = NULL;
            *queue = (struct audio_control_event_queue){0};
            ret = 0;
            break;
        }
    }
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return ret;
}

/** @brief Change subscription, clearing pending masks on disable or first enable.
 * @param queue Registered OFD storage, serialized against notifications.
 * @param enable Nonzero enables; repeated enable preserves pending events.
 * @return Zero, -EBADF, or -ENODEV; no callback, allocation, or sleep.
 */
int audio_control_queue_subscribe(struct audio_control_event_queue *queue, int enable)
{
    uint64_t flags;
    int ret = -EBADF;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    if (audio_control_queue_registered(queue)) {
        struct card_entry *c = find_card(queue->card);
        ret = -ENODEV;
        if (c && !c->closing) {
            if (!enable || !queue->subscribed)
                *queue = (struct audio_control_event_queue){.card = queue->card};
            queue->subscribed = enable != 0;
            ret = 0;
        }
    }
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return ret;
}

/** @brief Atomically inspect or dequeue the oldest merged element notification.
 * @param queue Registered OFD storage.
 * @param control Receives zero-based element index; must be writable.
 * @param mask Receives merged Linux event bits; must be writable.
 * @param consume Nonzero removes this item; later notifications remain pending.
 * @return Zero, -EINVAL, -EBADF, -EBADFD, -ENODEV, or -EAGAIN; IRQ safe.
 */
int audio_control_queue_next(struct audio_control_event_queue *queue,
                             uint32_t *control, uint32_t *mask, int consume)
{
    if (!control || !mask) return -EINVAL;
    uint64_t flags;
    int ret = -EBADF;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    if (audio_control_queue_registered(queue)) {
        struct card_entry *c = find_card(queue->card);
        if (!c || c->closing) ret = -ENODEV;
        else if (!queue->subscribed) ret = -EBADFD;
        else if (!queue->length) ret = -EAGAIN;
        else {
            *control = queue->order[queue->head];
            *mask = queue->pending[*control];
            if (consume) {
                queue->pending[*control] = 0;
                queue->head = (queue->head + 1) % AUDIO_CONTROL_EVENT_SLOTS;
                --queue->length;
            }
            ret = 0;
        }
    }
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return ret;
}
/** @brief Read period count of a live generation for diagnostics.
 * @param card Generation token.
 * @return Count, or zero for stale/disconnected card. IRQ safe, no ownership.
 */
uint64_t audio_card_periods(uint32_t card)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    struct card_entry *c = find_card(card); uint64_t result = c ? c->periods : 0;
    kernel_spin_unlock_irqrestore(&audio_lock, flags); return result;
}
/** @brief Copy one live card for dynamic /dev/snd directory enumeration. */
int audio_card_snapshot(uint32_t index, uint32_t *out_id,
                        struct audio_card_identity *identity)
{
    uint64_t flags;
    uint32_t seen = 0;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    for (uint32_t i = 0; i < CARD_MAX; ++i) {
        if (!cards[i].live || cards[i].closing) continue;
        if (seen++ != index) continue;
        if (out_id) *out_id = cards[i].id;
        if (identity) *identity = cards[i].identity;
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
        return 0;
    }
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return -2;
}
/** @brief Resolve a live generation token to its compact devfs card ordinal. */
int audio_card_index(uint32_t card)
{
    uint64_t flags;
    uint32_t seen = 0;
    kernel_spin_lock_irqsave(&audio_lock, &flags);
    for (uint32_t i = 0; i < CARD_MAX; ++i) {
        if (!cards[i].live || cards[i].closing) continue;
        if (cards[i].id == card) {
            kernel_spin_unlock_irqrestore(&audio_lock, flags);
            return (int)seen;
        }
        ++seen;
    }
    kernel_spin_unlock_irqrestore(&audio_lock, flags);
    return -2;
}

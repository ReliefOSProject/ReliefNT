#include "hda.h"

#include <linux/errno.h>
#include <stddef.h>

#define HDA_VERB_GET_PIN_SENSE 0xf09u
#define HDA_VERB_SET_UNSOLICITED_RESPONSE 0x708u

#define HDA_AMP_CAP_MUTE (1u << 31)
#define HDA_AMP_CAP_NUM_STEPS_SHIFT 8u
#define HDA_AMP_CAP_NUM_STEPS_MASK (0x7fu << HDA_AMP_CAP_NUM_STEPS_SHIFT)
#define HDA_AMP_SET_MUTE (1u << 7)
#define HDA_AMP_GAIN_MASK 0x7fu

/* The controls fixture links this translation unit without the full codec
 * implementation. A production link resolves the strong route symbol. */
extern int hda_apply_route(struct hda_codec *codec, struct hda_route *route,
                           bool enable) __attribute__((weak));

static int hda_controls_apply_route(struct hda_controls *controls,
                                     struct hda_route *route, bool enable)
{
    if (!hda_apply_route) return -ENOSYS;
    return hda_apply_route(controls->codec, route, enable);
}

static void hda_controls_name(char *dst, const char *src)
{
    uint32_t i = 0u;
    if (!dst) return;
    while (i + 1u < sizeof(((struct audio_control_info *)0)->name) &&
           src && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static const struct hda_codec_node *hda_controls_const_node(
    const struct hda_controls *controls, uint8_t nid)
{
    if (!controls || !controls->codec || !controls->codec->nodes) return NULL;
    if (!controls->codec->nodes[nid].present) return NULL;
    return &controls->codec->nodes[nid];
}

/** @brief Read capabilities of the selected real output amplifier.
 * @param controls Borrowed initialized control state.
 * @return Effective amp capability word or zero. No verbs or ownership change.
 */
static uint32_t hda_controls_output_caps(const struct hda_controls *controls)
{
    const struct hda_codec_node *node = controls ?
        hda_controls_const_node(controls, controls->playback_amp_nid) : NULL;
    return node ? node->amp_output_caps : 0u;
}

/** @brief Select an existing output amplifier along a resolved analog path.
 * @param controls Borrowed codec metadata.
 * @param route Optional resolved path, retained by its caller.
 * @param fallback Pin NID for callers without a path.
 * @return Actual amp NID or zero. Pure bounded topology lookup.
 */
static uint8_t hda_controls_output_nid(const struct hda_controls *controls,
                                       const struct hda_route *route,uint8_t fallback)
{
    if(route && route->path_count<=HDA_ROUTE_MAX_NODES)
        for(uint32_t n=0;n<route->path_count;n++){
            const struct hda_codec_node *node=hda_controls_const_node(controls,route->path[n]);
            if(node && (node->widget_caps&HDA_WCAP_OUT_AMP))return route->path[n];
        }
    const struct hda_codec_node *node=hda_controls_const_node(controls,fallback);
    return node && (node->widget_caps&HDA_WCAP_OUT_AMP)?fallback:0;
}

/** @brief Convert an HDA amplifier gain step to hundredths of a dB.
 * @param caps HDA amplifier capability word.
 * @param gain Raw integer gain step.
 * @return Signed hundredths of a dB using the capability offset and step size.
 * @par Context Pure task/IRQ-safe arithmetic with no hardware access.
 * @par Ownership and locks/IRQ No ownership, lock, allocation, or IRQ state.
 */
int32_t hda_amp_db(uint32_t caps, uint8_t gain)
{
    int32_t offset = (int32_t)(caps & 0x7fu);
    int32_t step = (int32_t)(((caps >> 16) & 0x7fu) + 1u);
    return ((int32_t)gain - offset) * step * 25;
}

/** @brief Acquire the control state without waiting for another transaction.
 * @param controls Live state retained by the caller.
 * @return True with ownership, false if another control/service operation owns it.
 * @par Context Task or bounded service; no wait or IRQ state change.
 */
static bool hda_controls_state_enter(struct hda_controls *controls)
{
    uint32_t expected=0;
    return __atomic_compare_exchange_n(&controls->busy,&expected,1,false,
                                       __ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
}

/** @brief Release the state transaction after publishing all scalar changes.
 * @param controls Live state whose gate belongs to the caller.
 * @return None. Release ordering; no hardware access or IRQ state change.
 */
static void hda_controls_state_leave(struct hda_controls *controls)
{ __atomic_store_n(&controls->busy,0,__ATOMIC_RELEASE); }

/** @brief Notify the core using the frozen registration index of a real element.
 * @param controls Live state under its gate, with the module/card retained.
 * @param control Internal HDA element ID, mapped before card publication.
 * @return None. Unpublished elements are ignored; no hardware wait occurs.
 */
static void hda_controls_notify(const struct hda_controls *controls,
                                uint32_t control)
{
    if (!controls || !controls->codec || !controls->codec->api ||
        !controls->codec->api->audio_control_changed || !controls->card_id ||
        control>=5u || controls->control_index[control]==UINT32_MAX)
        return;
    controls->codec->api->audio_control_changed(controls->card_id,
                                               controls->control_index[control]);
}

/** @brief Read only the channels physically implemented by one amplifier.
 * @param controls Live codec control state under its logical gate.
 * @param nid Real output amplifier NID.
 * @param left Receives left/mono value.
 * @param right Receives right value or a copy of mono.
 * @return 0 or verb errno. Waitable task context; no spinlock spans a verb.
 */
static int hda_controls_read_gain_locked(struct hda_controls *controls,
                                         uint8_t nid, uint16_t *left,
                                         uint16_t *right)
{
    int ret = hda_codec_control_read_amp(controls->codec, nid, false, 0u,
                                         false, left);
    if (ret) return ret;
    const struct hda_codec_node *node=hda_controls_const_node(controls,nid);
    if(node && !(node->widget_caps&HDA_WCAP_STEREO)){*right=*left;return 0;}
    return hda_codec_control_read_amp(controls->codec, nid, false, 0u,
                                      true, right);
}

/** @brief Restore actual amplifier channels after a failed stereo write.
 * @param controls Live state under the logical codec gate.
 * @param nid Real output amp.
 * @param left Original left/mono value.
 * @param right Original right value.
 * @param caps Effective amp caps.
 * @return 0 or errno. Waitable task context; mono has no right-channel write.
 */
static int hda_controls_restore_gain_locked(struct hda_controls *controls,
                                            uint8_t nid, uint16_t left,
                                            uint16_t right, uint32_t caps)
{
    int ret = hda_codec_control_write_amp(controls->codec, nid, false, 0u,
                                          false, left, caps);
    if (ret) return ret;
    const struct hda_codec_node *node=hda_controls_const_node(controls,nid);
    if(node && !(node->widget_caps&HDA_WCAP_STEREO))return 0;
    return hda_codec_control_write_amp(controls->codec, nid, false, 0u,
                                       true, right, caps);
}

/** @brief Write only implemented channels, restoring a failed stereo pair.
 * @param controls Live state under the codec logical gate.
 * @param nid Actual output amp.
 * @param left Requested left/mono value.
 * @param right Requested right value.
 * @param caps Effective amp capability word.
 * @return 0 or original errno. Task context; no ownership transfer.
 */
static int hda_controls_write_gain_locked(struct hda_controls *controls,
                                          uint8_t nid, uint16_t left,
                                          uint16_t right, uint32_t caps)
{
    uint16_t old_left = 0u, old_right = 0u;
    int ret = hda_controls_read_gain_locked(controls, nid, &old_left,
                                            &old_right);
    if (ret) return ret;
    const struct hda_codec_node *node=hda_controls_const_node(controls,nid);
    ret = hda_codec_control_write_amp(controls->codec, nid, false, 0u,
                                      false, left, caps);
    if (ret) return ret;
    if(node && !(node->widget_caps&HDA_WCAP_STEREO))return 0;
    ret = hda_codec_control_write_amp(controls->codec, nid, false, 0u,
                                      true, right, caps);
    if (ret) {
        int rollback = hda_controls_restore_gain_locked(controls, nid,
                                                         old_left, old_right,
                                                         caps);
        controls->last_error = ret;
        if (rollback) controls->last_error = ret;
        return ret;
    }
    return 0;
}

/** @brief Mute the actual output amplifier selected along a route.
 * @param controls Live state under the codec logical gate.
 * @param route Optional resolved analog path.
 * @param fallback_nid Pin fallback without a path.
 * @param mute Requested mute state.
 * @return 0 or errno. Task context; preserves actual gain values.
 */
static int hda_controls_write_route_mute_locked(struct hda_controls *controls,
                                                struct hda_route *route,
                                                uint8_t fallback_nid,
                                                bool mute)
{
    uint8_t nid = hda_controls_output_nid(controls,route,fallback_nid);
    const struct hda_codec_node *node = hda_controls_const_node(controls, nid);
    if (!node || !(node->widget_caps & HDA_WCAP_OUT_AMP)) return -ENODEV;
    uint16_t left = 0u, right = 0u;
    int ret = hda_controls_read_gain_locked(controls, nid, &left, &right);
    if (ret) return ret;
    uint16_t mask = mute ? HDA_AMP_SET_MUTE : 0u;
    return hda_controls_write_gain_locked(controls, nid,
                                          (uint16_t)((left & ~HDA_AMP_SET_MUTE) | mask),
                                          (uint16_t)((right & ~HDA_AMP_SET_MUTE) | mask),
                                          node->amp_output_caps);
}

/** @brief Apply jack policy to unbound routes or a speaker amplifier.
 * @param controls Live caller-owned state under its nonwaiting transaction gate.
 * @return Zero or original route/amp errno; rollback failure is kept in last_error.
 * @par Context Waitable task only. Bound routes require a stream-owner transaction.
 * @par Ownership Caller retains both routes. No spinlock spans a verb; rollback
 * disables the replacement before restoring the original shared signal path.
 */
static int hda_controls_apply_auto_mute(struct hda_controls *controls)
{
    if (!controls) return -EINVAL;
    if(controls->apply_policy)
        return controls->apply_policy(controls->policy_opaque,controls->auto_mute,
                                      controls->headphone_present);
    bool want_headphone=controls->auto_mute && controls->headphone_present;
    struct hda_route *speaker=controls->speaker_route,*headphone=controls->headphone_route;
    if (!speaker || !speaker->path_count || !headphone || !headphone->path_count) {
        int ret=hda_codec_control_gate_enter(controls->codec);
        if(ret)return ret;
        ret=hda_controls_write_route_mute_locked(controls,speaker,controls->speaker_pin,
                                                 want_headphone || controls->playback_mute);
        hda_codec_control_gate_leave(controls->codec);
        if(!ret)controls->policy_mute=want_headphone;
        else controls->last_error=ret;
        return ret;
    }
    /* Route apply/restore does not own the SD RUN bit or converter tag. */
    if(speaker->stream_bound || speaker->stream_bind_dirty ||
       headphone->stream_bound || headphone->stream_bind_dirty)return -EBUSY;
    uint8_t old_speaker=speaker->active,old_headphone=headphone->active;
    uint8_t old_nid=controls->playback_amp_nid;
    struct hda_route *target=want_headphone?headphone:speaker;
    uint8_t target_nid=hda_controls_output_nid(controls,target,target->pin_nid);
    const struct hda_codec_node *old_amp=hda_controls_const_node(controls,old_nid);
    const struct hda_codec_node *target_amp=hda_controls_const_node(controls,target_nid);
    if(!old_amp || !target_amp)return -ENODEV;
    /* Public volume metadata stays fixed while the physical path changes. */
    if(old_amp->amp_output_caps!=target_amp->amp_output_caps ||
       ((old_amp->widget_caps^target_amp->widget_caps)&HDA_WCAP_STEREO))return -EOPNOTSUPP;
    uint16_t old_left=0,old_right=0;
    int ret=hda_codec_control_gate_enter(controls->codec);
    if(ret)return ret;
    ret=hda_controls_read_gain_locked(controls,old_nid,&old_left,&old_right);
    hda_codec_control_gate_leave(controls->codec);
    if(ret)return ret;
    if(want_headphone && speaker->active){
        ret=hda_controls_apply_route(controls,speaker,false);if(ret)goto restore;
    }
    if(!want_headphone && headphone->active){
        ret=hda_controls_apply_route(controls,headphone,false);if(ret)goto restore;
    }
    if(!target->active){ret=hda_controls_apply_route(controls,target,true);if(ret)goto restore;}
    /* Disabling the speaker route already isolates its pin. Muting a shared DAC
     * here would also silence the newly active headphone path. */
    ret=hda_codec_control_gate_enter(controls->codec);
    if(ret)goto restore;
    ret=hda_controls_write_gain_locked(controls,target_nid,old_left,old_right,target_amp->amp_output_caps);
    hda_codec_control_gate_leave(controls->codec);
    if(ret)goto restore;
    controls->playback_amp_nid=target_nid;
    controls->policy_mute=0;
    return 0;
restore:
    {
        int original=ret,rollback=0,next;
        if(headphone->active && !old_headphone){
            next=hda_controls_apply_route(controls,headphone,false);if(next && !rollback)rollback=next;
        }
        if(speaker->active && !old_speaker){
            next=hda_controls_apply_route(controls,speaker,false);if(next && !rollback)rollback=next;
        }
        if(old_speaker && !speaker->active){
            next=hda_controls_apply_route(controls,speaker,true);if(next && !rollback)rollback=next;
        }
        if(old_headphone && !headphone->active){
            next=hda_controls_apply_route(controls,headphone,true);if(next && !rollback)rollback=next;
        }
        if(speaker->active==old_speaker && headphone->active==old_headphone){
            next=hda_codec_control_gate_enter(controls->codec);
            if(!next){
                next=hda_controls_restore_gain_locked(controls,old_nid,old_left,old_right,old_amp->amp_output_caps);
                hda_codec_control_gate_leave(controls->codec);
            }
            if(next && !rollback)rollback=next;
        }
        controls->last_error=rollback?rollback:original;
        return original;
    }
}

/** @brief Enable a jack tag through the twelve-bit unsolicited response verb.
 * @param controls Live caller-owned mixer/jack metadata.
 * @return None. Task init; transport failure selects bounded polling.
 */
static void hda_controls_configure_unsolicited(struct hda_controls *controls)
{
    struct hda_controller *controller;
    uint32_t response = 0u;
    int ret;
    if (!controls || !controls->codec || !(controller = controls->codec->controller)) {
        if (controls) controls->poll_only = 1u;
        return;
    }
    if (controller->transport_mode != HDA_TRANSPORT_RINGS ||
        !controller->rings_started || !controls->headphone_pin ||
        !hda_controls_const_node(controls, controls->headphone_pin) ||
        !(hda_controls_const_node(controls, controls->headphone_pin)->widget_caps &
          HDA_WCAP_UNSOLICITED)) {
        controls->poll_only = 1u;
        return;
    }
    ret = hda_exec_verb(controller, controls->codec->cad,
                        controls->headphone_pin,
                        HDA_VERB_SET_UNSOLICITED_RESPONSE,
                        (uint16_t)(HDA_UNSOLICITED_ENABLE | 1u), false,
                        &response);
    if (ret) {
        controls->poll_only = 1u;
        controls->last_error = ret;
        return;
    }
    controls->headphone_tag = 1u;
    controls->unsolicited_enabled = 1u;
    controls->next_poll_tick = 0u;
}

/** @brief Initialize mixer/jack state from actual amplifier readback.
 * @param controls Caller-owned control state.
 * @param codec Live codec cache.
 * @param speaker_route Resolved speaker route, or NULL when speaker_pin is used.
 * @param headphone_route Optional headphone route.
 * @param speaker_pin Playback pin NID.
 * @param headphone_pin Jack pin NID.
 * @param capture_route Optional capture route.
 * @param capture_source_count Number of real capture sources.
 * @param auto_mute Initial auto-mute policy.
 * @return 0 after amplifier readback or valid jack setup without an amp;
 * otherwise the first validation/transport error.
 * @par Context Task context during card setup.
 * @par Ownership and locks/IRQ Caller retains borrowed codec/routes; codec gate
 * is held only around synchronous amplifier reads.
 */
int hda_controls_init(struct hda_controls *controls, struct hda_codec *codec,
                      struct hda_route *speaker_route,
                      struct hda_route *headphone_route, uint8_t speaker_pin,
                      uint8_t headphone_pin, struct hda_route *capture_route,
                      uint8_t capture_source_count, bool auto_mute)
{
    if (!controls || !codec || !codec->nodes ||
        (!speaker_route && !speaker_pin)) return -EINVAL;
    __builtin_memset(controls, 0, sizeof(*controls));
    for(unsigned n=0;n<5u;n++)controls->control_index[n]=n;
    controls->codec = codec;
    controls->speaker_route = speaker_route;
    controls->headphone_route = headphone_route;
    controls->capture_route = capture_route;
    controls->speaker_pin = speaker_route ? speaker_route->pin_nid : speaker_pin;
    controls->playback_amp_nid=hda_controls_output_nid(controls,speaker_route,controls->speaker_pin);
    controls->headphone_pin = headphone_route ? headphone_route->pin_nid : headphone_pin;
    const struct hda_codec_node *jack = hda_controls_const_node(controls, controls->headphone_pin);
    if (!jack || jack->type != HDA_WIDGET_PIN ||
        !(jack->pin_caps & HDA_PINCAP_PRESENCE)) controls->headphone_pin = 0;
    controls->sense_pending = controls->headphone_pin != 0;
    controls->capture_source_count = capture_source_count ? capture_source_count : 1u;
    controls->auto_mute = auto_mute ? 1u : 0u;
    controls->card_id = codec->controller ? codec->controller->card_id : 0u;

    const struct hda_codec_node *node = hda_controls_const_node(
        controls, controls->playback_amp_nid);
    if (!node || !(node->widget_caps & HDA_WCAP_OUT_AMP)) {
        if (!controls->headphone_pin) return -ENODEV;
        hda_controls_configure_unsolicited(controls);
        return 0;
    }
    int ret = hda_codec_control_gate_enter(codec);
    if (ret) return ret;
    uint16_t left = 0u, right = 0u;
    ret = hda_controls_read_gain_locked(controls, controls->playback_amp_nid,
                                        &left, &right);
    hda_codec_control_gate_leave(codec);
    if (ret) return ret;
    controls->playback_gain[0] = (uint8_t)(left & HDA_AMP_GAIN_MASK);
    controls->playback_gain[1] = (uint8_t)(right & HDA_AMP_GAIN_MASK);
    controls->playback_mute = (node->amp_output_caps & HDA_AMP_CAP_MUTE) &&
                              ((left & HDA_AMP_SET_MUTE) != 0u);
    hda_controls_configure_unsolicited(controls);
    return 0;
}

/** @brief Clear task-owned control state without touching borrowed hardware.
 * @param controls Control state to clear.
 * @return None.
 * @par Context Task context after service cancellation.
 * @par Ownership and locks/IRQ No lock, allocation, IRQ, or hardware access.
 */
void hda_controls_destroy(struct hda_controls *controls)
{
    if (!controls) return;
    __builtin_memset(controls, 0, sizeof(*controls));
}

/** @brief Return the fixed registration-order control count.
 * @param controls Initialized immutable control metadata.
 * @return Five for a live state, otherwise zero.
 * @par Context Any nonblocking card metadata context.
 * @par Ownership and locks/IRQ Read-only scalar access with no lock or IRQ work.
 */
uint32_t hda_controls_count(const struct hda_controls *controls)
{
    return controls ? 5u : 0u;
}

/** @brief Describe one real mixer, source, jack, or policy control.
 * @param controls Initialized control state.
 * @param control Internal HDA element ID.
 * @param info Receives copied metadata.
 * @return 0, -ENODEV for absent amp, or -EINVAL for an invalid state/id.
 * @par Context Task context outside the core registry lock.
 * @par Ownership and locks/IRQ No hardware access, allocation, lock, or IRQ work.
 */
int hda_controls_info(const struct hda_controls *controls, uint32_t control,
                     struct audio_control_info *info)
{
    if (!controls || !info || control >= hda_controls_count(controls))
        return -EINVAL;
    __builtin_memset(info, 0, sizeof(*info));
    info->id = control;
    uint32_t caps = hda_controls_output_caps(controls);
    if ((control==HDA_CONTROL_PLAYBACK_VOLUME || control==HDA_CONTROL_PLAYBACK_MUTE) &&
        !controls->playback_amp_nid) return -ENODEV;
    switch (control) {
    case HDA_CONTROL_PLAYBACK_VOLUME:
        info->type = AUDIO_CONTROL_INTEGER;
        const struct hda_codec_node *amp=hda_controls_const_node(controls,controls->playback_amp_nid);
        info->count = amp && (amp->widget_caps&HDA_WCAP_STEREO)?2u:1u;
        info->access = AUDIO_CONTROL_READ | AUDIO_CONTROL_WRITE;
        info->min = 0;
        info->max = (int64_t)((caps >> 8) & 0x7fu);
        info->step = 1;
        info->db_min = hda_amp_db(caps, 0u);
        info->db_step = hda_amp_db(caps, 1u) - info->db_min;
        hda_controls_name(info->name, "Playback Volume");
        return 0;
    case HDA_CONTROL_PLAYBACK_MUTE:
        info->type = AUDIO_CONTROL_BOOLEAN;
        info->count = 1u;
        info->access = AUDIO_CONTROL_READ |
                       ((caps & HDA_AMP_CAP_MUTE) ? AUDIO_CONTROL_WRITE : 0u);
        info->min = 0;
        info->max = 1;
        info->step = 1;
        hda_controls_name(info->name, "Playback Mute");
        return 0;
    case HDA_CONTROL_CAPTURE_SOURCE:
        info->type = AUDIO_CONTROL_ENUMERATED;
        info->count = 1u;
        info->access = AUDIO_CONTROL_READ | AUDIO_CONTROL_WRITE;
        info->min = 0;
        info->max = (int64_t)controls->capture_source_count - 1;
        info->step = 1;
        info->items = controls->capture_source_count;
        if (info->items > 16u) info->items = 16u;
        for (uint32_t i = 0u; i < info->items; ++i)
            hda_controls_name(info->item_names[i], i == 0u ? "Mic" : "Line");
        hda_controls_name(info->name, "Capture Source");
        return 0;
    case HDA_CONTROL_HEADPHONE_JACK:
        info->type = AUDIO_CONTROL_BOOLEAN;
        info->count = 1u;
        info->access = AUDIO_CONTROL_READ | AUDIO_CONTROL_VOLATILE;
        info->min = 0;
        info->max = 1;
        info->step = 1;
        hda_controls_name(info->name, "Headphone Jack");
        return 0;
    case HDA_CONTROL_AUTO_MUTE:
        info->type = AUDIO_CONTROL_BOOLEAN;
        info->count = 1u;
        info->access = AUDIO_CONTROL_READ | AUDIO_CONTROL_WRITE;
        info->min = 0;
        info->max = 1;
        info->step = 1;
        hda_controls_name(info->name, "Auto-Mute");
        return 0;
    default:
        return -EINVAL;
    }
}

/** @brief Read a hardware-backed or policy-backed control.
 * @param controls Initialized control state.
 * @param control Internal HDA element ID.
 * @param value Receives bounded control values.
 * @return 0 or a validation/transport error.
 * @par Context Task context without the core registry lock.
 * @par Ownership and locks/IRQ Codec gate ownership is local to this operation;
 * no lock spans the synchronous verb.
 */
static int hda_controls_read_locked(struct hda_controls *controls, uint32_t control,
                      struct audio_control_value *value)
{
    if (!controls || !value || control >= hda_controls_count(controls))
        return -EINVAL;
    __builtin_memset(value, 0, sizeof(*value));
    if (control == HDA_CONTROL_PLAYBACK_VOLUME ||
        control == HDA_CONTROL_PLAYBACK_MUTE) {
        if (!controls->playback_amp_nid) return -ENODEV;
        uint32_t caps = hda_controls_output_caps(controls);
        if (control == HDA_CONTROL_PLAYBACK_MUTE &&
            !(caps & HDA_AMP_CAP_MUTE)) {
            value->values[0] = 0;
            return 0;
        }
        int ret = hda_codec_control_gate_enter(controls->codec);
        if (ret) return ret;
        uint16_t left = 0u, right = 0u;
        ret = hda_controls_read_gain_locked(controls, controls->playback_amp_nid,
                                             &left, &right);
        hda_codec_control_gate_leave(controls->codec);
        if (ret) return ret;
        controls->playback_gain[0] = (uint8_t)(left & HDA_AMP_GAIN_MASK);
        controls->playback_gain[1] = (uint8_t)(right & HDA_AMP_GAIN_MASK);
        if(!controls->policy_mute)
            controls->playback_mute = (left & HDA_AMP_SET_MUTE) != 0u;
        if (control == HDA_CONTROL_PLAYBACK_VOLUME) {
            value->values[0] = controls->playback_gain[0];
            value->values[1] = controls->playback_gain[1];
        } else {
            value->values[0] = controls->playback_mute;
        }
        return 0;
    }
    if (control == HDA_CONTROL_CAPTURE_SOURCE)
        value->values[0] = controls->capture_source;
    else if (control == HDA_CONTROL_HEADPHONE_JACK)
        value->values[0] = controls->headphone_present;
    else
        value->values[0] = controls->auto_mute;
    return 0;
}

/** @brief Atomically write a gain, mute, source, or auto-mute control.
 * @param controls Initialized control state.
 * @param control Internal HDA element ID.
 * @param value Caller-owned bounded control values.
 * @return 0 after readback, or the original error after rollback.
 * @par Context Task context only; synchronous verbs are forbidden in service/IRQ.
 * @par Ownership and locks/IRQ Codec gate serializes route/control operations;
 * rollback retains the prior hardware values on failure.
 */
static int hda_controls_write_locked(struct hda_controls *controls, uint32_t control,
                       const struct audio_control_value *value)
{
    if (!controls || !value || control >= hda_controls_count(controls))
        return -EINVAL;
    uint32_t caps = hda_controls_output_caps(controls);
    if (control == HDA_CONTROL_PLAYBACK_VOLUME ||
        control == HDA_CONTROL_PLAYBACK_MUTE) {
        if (!controls->playback_amp_nid) return -ENODEV;
        if (control == HDA_CONTROL_PLAYBACK_MUTE &&
            !(caps & HDA_AMP_CAP_MUTE)) return -EOPNOTSUPP;
        int64_t max_gain = (int64_t)((caps >> 8) & 0x7fu);
        int64_t left_gain = control == HDA_CONTROL_PLAYBACK_VOLUME ?
                            value->values[0] : controls->playback_gain[0];
        int64_t right_gain = control == HDA_CONTROL_PLAYBACK_VOLUME ?
                             value->values[1] : controls->playback_gain[1];
        const struct hda_codec_node *amp=hda_controls_const_node(controls,controls->playback_amp_nid);
        if(amp && !(amp->widget_caps&HDA_WCAP_STEREO))right_gain=left_gain;
        if (left_gain < 0 || right_gain < 0 || left_gain > max_gain ||
            right_gain > max_gain ||
            (control == HDA_CONTROL_PLAYBACK_MUTE &&
             (value->values[0] < 0 || value->values[0] > 1))) return -ERANGE;
        int ret = hda_codec_control_gate_enter(controls->codec);
        if (ret) return ret;
        uint16_t old_left = 0u, old_right = 0u;
        ret = hda_controls_read_gain_locked(controls, controls->playback_amp_nid,
                                            &old_left, &old_right);
        if (!ret) {
            uint16_t mute = control == HDA_CONTROL_PLAYBACK_MUTE &&
                            (value->values[0] || controls->policy_mute) ? HDA_AMP_SET_MUTE :
                            (control == HDA_CONTROL_PLAYBACK_MUTE ? 0u :
                             (old_left & HDA_AMP_SET_MUTE));
            uint16_t rmute = control == HDA_CONTROL_PLAYBACK_MUTE &&
                             (value->values[0] || controls->policy_mute) ? HDA_AMP_SET_MUTE :
                             (control == HDA_CONTROL_PLAYBACK_MUTE ? 0u :
                              (old_right & HDA_AMP_SET_MUTE));
            ret = hda_controls_write_gain_locked(
                controls, controls->playback_amp_nid,
                (uint16_t)((uint16_t)left_gain | mute),
                (uint16_t)((uint16_t)right_gain | rmute), caps);
        }
        hda_codec_control_gate_leave(controls->codec);
        if (ret) return ret;
        controls->playback_gain[0] = (uint8_t)left_gain;
        controls->playback_gain[1] = (uint8_t)right_gain;
        if (control == HDA_CONTROL_PLAYBACK_MUTE)
            controls->playback_mute = value->values[0] != 0;
        hda_controls_notify(controls, control);
        return 0;
    }
    if (control == HDA_CONTROL_CAPTURE_SOURCE) {
        if (value->values[0] < 0 ||
            value->values[0] >= controls->capture_source_count)
            return -ERANGE;
        controls->capture_source = (uint8_t)value->values[0];
        hda_controls_notify(controls, control);
        return 0;
    }
    if (control == HDA_CONTROL_HEADPHONE_JACK) return -EACCES;
    if (value->values[0] < 0 || value->values[0] > 1) return -ERANGE;
    uint8_t old_auto = controls->auto_mute;
    controls->auto_mute = value->values[0] != 0;
    if (old_auto != controls->auto_mute || controls->auto_mute_pending) {
        int ret = hda_controls_apply_auto_mute(controls);
        if (ret) {
            controls->auto_mute = old_auto;
            return ret;
        }
        controls->auto_mute_pending=0;
    }
    hda_controls_notify(controls, control);
    return 0;
}

/** @brief Deduplicate one raw pin-sense response and queue policy work.
 * @param controls Borrowed control state.
 * @param sense Raw GET_PIN_SENSE response.
 * @return 1 for a state change, 0 for a duplicate, or -EINVAL for NULL state.
 * @par Context Nonblocking service or task context; no synchronous verb.
 * @par Ownership and locks/IRQ Route work is deferred to the worker; no lock or
 * IRQ operation occurs.
 */
static int hda_controls_process_jack_sense_locked(struct hda_controls *controls,
                                    uint32_t sense)
{
    if (!controls) return -EINVAL;
    uint8_t present = (sense & HDA_PIN_SENSE_PRESENCE) != 0u;
    if (present == controls->headphone_present) return 0;
    controls->headphone_present = present;
    ++controls->jack_events;
    controls->auto_mute_pending = controls->auto_mute;
    hda_controls_notify(controls, HDA_CONTROL_HEADPHONE_JACK);
    return 1;
}

/** @brief Share a bounded budget across transport, own-CAD events and sense.
 * @param opaque Borrowed struct hda_controls.
 * @param budget Maximum transport entries, own-CAD events and sense progress units.
 * @return Work consumed, bounded by budget.
 * @par Context Audio service/tick or task context; never waits for a verb.
 * @par Ownership and locks/IRQ Controller owns the queue and flight; this path
 * uses only bounded submit/poll operations and no allocation or sleep.
 */
static uint32_t hda_controls_service_locked(void *opaque, uint32_t budget)
{
    struct hda_controls *controls = opaque;
    if (!controls || !controls->codec || !controls->codec->controller ||
        !controls->headphone_pin || !budget)
        return 0u;
    if (budget > HDA_SERVICE_BUDGET) budget = HDA_SERVICE_BUDGET;
    struct hda_controller *controller = controls->codec->controller;
    uint32_t work = hda_controller_service_budget(controller, budget);
    struct hda_unsolicited_response event;
    while (work < budget &&
           !hda_unsolicited_pop_for_codec(controller, controls->codec->cad, &event)) {
        uint8_t tag = (uint8_t)((event.response >> 26) & HDA_UNSOLICITED_TAG_MASK);
        if (tag == controls->headphone_tag || tag == controls->speaker_tag)
            controls->sense_pending = 1u;
        ++work;
    }
    uint32_t dropped = hda_unsolicited_dropped_count(controller);
    if (dropped != controls->unsol_dropped_seen) {
        controls->unsol_dropped_seen = dropped;
        controls->sense_pending = 1u;
    }
    if (work >= budget) return work;
    uint64_t now = controller->api && controller->api->ticks ?
                   controller->api->ticks() : 0u;
    if (controls->sense_inflight) {
        uint32_t sense = 0u;
        int ret = hda_verb_poll(controller, controls->sense_ticket, &sense);
        if (!ret) {
            controls->sense_inflight = 0u;
            (void)hda_controls_process_jack_sense_locked(controls, sense);
            if (controls->poll_only) controls->next_poll_tick = now + 10u;
            ++work;
        } else if (ret != -EAGAIN) {
            controls->sense_inflight = 0u;
            controls->sense_pending = 1u;
            controls->last_error = ret;
            ++work;
        }
    } else if (controls->poll_only && (controls->sense_pending ||
               (controls->next_poll_tick == 0u || now >= controls->next_poll_tick))) {
        uint64_t ticket = 0u;
        int ret = hda_verb_submit(controller, controls->codec->cad,
                                  controls->headphone_pin,
                                  HDA_VERB_GET_PIN_SENSE, 0u, false, &ticket);
        if (!ret) {
            controls->sense_ticket = ticket;
            controls->sense_inflight = 1u;
            controls->sense_pending = 0u;
            controls->next_poll_tick = now + 10u;
            ++work;
        } else if (ret != -EBUSY) {
            controls->last_error = ret;
        }
    } else if (!controls->poll_only && controls->sense_pending) {
        uint64_t ticket = 0u;
        int ret = hda_verb_submit(controller, controls->codec->cad,
                                  controls->headphone_pin,
                                  HDA_VERB_GET_PIN_SENSE, 0u, false, &ticket);
        if (!ret) {
            controls->sense_ticket = ticket;
            controls->sense_inflight = 1u;
            controls->sense_pending = 0u;
            ++work;
        } else if (ret != -EBUSY) {
            controls->last_error = ret;
        }
    }
    return work > budget ? budget : work;
}

/** @brief Run nonblocking service and apply deferred auto-mute task work.
 * @param controls Borrowed live control state.
 * @param budget Bounded service budget.
 * @return 0 or the route/amp error from deferred policy work. Zero budget
 * preserves all work; failed policy application remains queued for retry.
 * @par Context Task context after the service pass.
 * @par Ownership and locks/IRQ Codec gate is held only around direct amp work;
 * no core lock or IRQ handler is blocked.
 */
int hda_controls_worker(struct hda_controls *controls, uint32_t budget)
{
    if (!controls) return -EINVAL;
    if (!budget) return 0;
    if (!hda_controls_state_enter(controls)) return -EBUSY;
    int ret=0;
    if (hda_controls_service_locked(controls, budget)<budget && controls->auto_mute_pending) {
        ret=hda_controls_apply_auto_mute(controls);
        if (!ret) controls->auto_mute_pending=0u;
    }
    hda_controls_state_leave(controls);
    return ret;
}

/** @brief Read one control while serializing its cached state with jack service.
 * @param controls Borrowed live control state.
 * @param control Internal HDA element ID.
 * @param value Receives hardware/scalar values on success.
 * @return Zero, validation/verb errno, or -EBUSY without waiting on contention.
 * @par Context Waitable task context; no spinlock or core lock spans verbs.
 */
int hda_controls_read(struct hda_controls *controls,uint32_t control,
                       struct audio_control_value *value)
{
    if (!controls || !value) return -EINVAL;
    if (!hda_controls_state_enter(controls)) return -EBUSY;
    int ret=hda_controls_read_locked(controls,control,value);
    hda_controls_state_leave(controls);return ret;
}

/** @brief Write one control without racing the pending jack/policy transaction.
 * @param controls Borrowed live control state.
 * @param control Internal HDA element ID.
 * @param value Borrowed requested values.
 * @return Zero, validation/verb errno, or -EBUSY without touching busy state.
 * @par Context Waitable task context; caller retains the module and routes.
 */
int hda_controls_write(struct hda_controls *controls,uint32_t control,
                        const struct audio_control_value *value)
{
    if (!controls || !value) return -EINVAL;
    if (!hda_controls_state_enter(controls)) return -EBUSY;
    int ret=hda_controls_write_locked(controls,control,value);
    hda_controls_state_leave(controls);return ret;
}

/** @brief Publish one jack sample under the nonwaiting state transaction gate.
 * @param controls Borrowed live control state.
 * @param sense Raw hardware pin-sense response.
 * @return One for change, zero for duplicate, or -EINVAL/-EBUSY without mutation.
 * @par Context Task or bounded service; no synchronous verb or IF change.
 */
int hda_controls_process_jack_sense(struct hda_controls *controls,uint32_t sense)
{
    if (!controls) return -EINVAL;
    if (!hda_controls_state_enter(controls)) return -EBUSY;
    int ret=hda_controls_process_jack_sense_locked(controls,sense);
    hda_controls_state_leave(controls);return ret;
}

/** @brief Service one jack flight without racing a waitable control transaction.
 * @param opaque Borrowed live control state, pinned by its caller.
 * @param budget Shared transport/event/sense limit.
 * @return Actual work bounded by budget; zero on contention preserves pending work.
 * @par Context Task or bounded service; no wait, spinlock, allocation or IF change.
 */
uint32_t hda_controls_service(void *opaque,uint32_t budget)
{
    struct hda_controls *controls=opaque;
    if (!controls || !budget || !hda_controls_state_enter(controls)) return 0;
    uint32_t work=hda_controls_service_locked(controls,budget);
    hda_controls_state_leave(controls);return work;
}

/** @brief Share the task budget between sensing and one queued route transaction.
 * @param opaque Borrowed controls pinned with the owning card/stream metadata.
 * @param budget Maximum work units including at most one policy attempt.
 * @return Actual bounded work; contention leaves all queued state intact.
 * @par Context Waitable task without execution ownership or IRQ/core spinlocks.
 * @par Ownership The enclosing caller serializes stream mutations. Policy errors
 * stay observable and pending; this function never runs from the IRQ service.
 */
uint32_t hda_controls_task_service(void *opaque,uint32_t budget)
{
    struct hda_controls *controls=opaque;
    if(!controls || !budget || !hda_controls_state_enter(controls))return 0;
    if(budget>HDA_SERVICE_BUDGET)budget=HDA_SERVICE_BUDGET;
    uint32_t work=hda_controls_service_locked(controls,budget);
    if(work<budget && controls->auto_mute_pending){
        int ret=hda_controls_apply_auto_mute(controls);
        if(!ret)controls->auto_mute_pending=0;
        else controls->last_error=ret;
        ++work;
    }
    hda_controls_state_leave(controls);return work;
}

/** @brief Adapt fixed control count to the audio core opaque callback ABI.
 * @param opaque Borrowed struct hda_controls.
 * @return Stable control count, or zero for NULL.
 * @par Context Task metadata query with no hardware access.
 * @par Ownership and locks/IRQ No ownership transfer, lock, allocation, or IRQ work.
 */
uint32_t hda_controls_card_count(void *opaque)
{
    return hda_controls_count((const struct hda_controls *)opaque);
}

/** @brief Adapt control metadata to the audio core opaque callback ABI.
 * @param opaque Borrowed struct hda_controls.
 * @param control Internal HDA element ID.
 * @param info Receives copied metadata.
 * @return 0 or the metadata validation error.
 * @par Context Task context outside the registry lock.
 * @par Ownership and locks/IRQ No ownership transfer or hardware access.
 */
int hda_controls_card_info(void *opaque, uint32_t control,
                           struct audio_control_info *info)
{
    return hda_controls_info((const struct hda_controls *)opaque, control, info);
}

/** @brief Adapt control reads to the audio core opaque callback ABI.
 * @param opaque Borrowed struct hda_controls.
 * @param control Internal HDA element ID.
 * @param value Receives the bounded control value.
 * @return 0 or the read validation/transport error.
 * @par Context Task context without the registry lock.
 * @par Ownership and locks/IRQ Codec gate ownership remains internal.
 */
int hda_controls_card_read(void *opaque, uint32_t control,
                           struct audio_control_value *value)
{
    return hda_controls_read((struct hda_controls *)opaque, control, value);
}

/** @brief Adapt control writes to the audio core opaque callback ABI.
 * @param opaque Borrowed struct hda_controls.
 * @param control Internal HDA element ID.
 * @param value Caller-owned bounded control value.
 * @return 0 or the write validation/rollback error.
 * @par Context Task context without the registry lock.
 * @par Ownership and locks/IRQ Synchronous codec work stays inside the control transaction.
 */
int hda_controls_card_write(void *opaque, uint32_t control,
                            const struct audio_control_value *value)
{
    return hda_controls_write((struct hda_controls *)opaque, control, value);
}

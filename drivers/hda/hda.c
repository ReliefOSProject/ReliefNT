#include "hda.h"
#include <linux/errno.h>
#include <stddef.h>

#define HDA_MODULE_CONTROLLERS 16u
#define HDA_MODULE_CARDS 16u
#define HDA_MODULE_DEVICES 8u
struct hda_module_controller;
struct hda_endpoint {
    struct hda_route_group group;
    struct hda_route capture;
    struct hda_stream *lease;
    uint8_t is_capture;
};
struct hda_module_card {
    struct hda_module_card *next;
    struct hda_module_controller *owner;
    uint64_t phys;
    uint32_t pages, id;
    struct hda_codec codec;
    struct hda_endpoint playback[HDA_MODULE_DEVICES], capture[HDA_MODULE_DEVICES];
    uint32_t playback_count, capture_count;
    struct hda_controls controls;
    uint8_t control_ids[5];
    uint8_t control_count, controls_ready, capture_source;
    uint32_t operation_busy;
    struct hda_endpoint *policy_speaker, *policy_headphone, *policy_selected;
};
struct hda_module_controller {
    struct hda_module_controller *next;
    struct hda_module_card *cards;
    uint64_t phys;
    uint32_t pages;
    struct hda_controller hw;
    struct hda_stream streams[HDA_STREAM_MAX];
};
static const struct reliefos_driver_kernel_api *kernel_api;
static struct hda_module_controller *controllers;
static uint32_t published_cards;

/** @brief Log one bounded discovery failure with stable BDF/codec identifiers.
 * @param stage Static operation name.
 * @param dev Borrowed PCI identity.
 * @param cad Codec address, zero for controller operations.
 * @param error Negative errno.
 * @return None. Task-only diagnostic without physical or virtual addresses.
 */
static void hda_module_error(const char *stage,const struct reliefos_driver_pci_device *dev,
                             uint8_t cad,int error)
{
    char line[120];unsigned n=0;const char *prefix="[hda] ";const char *hex="0123456789abcdef";
    while(*prefix)line[n++]=*prefix++;
    while(*stage && n<64)line[n++]=*stage++;
    const char *label=" bdf=";while(*label)line[n++]=*label++;
    line[n++]=hex[dev->bus>>4];line[n++]=hex[dev->bus&15];line[n++]=':';
    line[n++]=hex[dev->slot>>4];line[n++]=hex[dev->slot&15];line[n++]='.';
    line[n++]=hex[dev->function&15];label=" cad=";while(*label)line[n++]=*label++;
    line[n++]=hex[cad&15];label=" errno=";while(*label)line[n++]=*label++;
    uint32_t value=(uint32_t)error;if(error<0){line[n++]='-';value=0u-value;}
    char digits[10];unsigned count=0;do{digits[count++]=(char)('0'+value%10);value/=10;}while(value);
    while(count)line[n++]=digits[--count];line[n++]='\n';line[n]=0;kernel_api->console_write(line);
}

/** @brief Clear module-local bytes without an external libc dependency.
 * @param dst Borrowed writable storage.
 * @param value Byte value.
 * @param bytes Number of bytes.
 * @return Original dst. No allocation, lock, or ownership change.
 */
void *memset(void *dst, int value, size_t bytes)
{
    volatile unsigned char *p=dst;
    while(bytes--)*p++=(unsigned char)value;
    return dst;
}

/** @brief Forward stream completion through the size-checked module API.
 * @param card Core card generation token.
 * @param stream Hardware stream generation token.
 * @param frames Completed frame count.
 * @param error Negative stream error or zero.
 * @return None. Bounded IRQ callback; no kernel symbol relocation is required.
 */
void audio_period_elapsed(uint32_t card,uint32_t stream,uint64_t frames,int error)
{
    if(kernel_api)kernel_api->audio_period_elapsed(card,stream,frames,error);
}

/** @brief Allocate and validate an exact run of CPU metadata pages.
 * @param bytes Metadata byte size.
 * @param phys Receives the owned physical run.
 * @param pages Receives its exact page count.
 * @return Zeroed direct-map pointer or NULL. Task init, no state lock held.
 */
static void *hda_module_alloc(size_t bytes,uint64_t *phys,uint32_t *pages)
{
    *pages=(uint32_t)((bytes+4095u)/4096u);
    *phys=kernel_api->alloc_pages(*pages);
    if(!*phys)return NULL;
    void *p=hda_phys_to_direct_map_page(*phys);
    if(!p || !hda_phys_to_direct_map_page(*phys+((uint64_t)*pages-1u)*4096u)){
        kernel_api->free_pages(*phys,*pages);*phys=0;return NULL;
    }
    memset(p,0,(size_t)*pages*4096u);return p;
}

/** @brief Resolve one logical device to its direction-specific codec endpoint.
 * @param card Borrowed live card.
 * @param device Logical PCM number.
 * @param direction Playback or capture.
 * @return Endpoint or NULL for unavailable direction/device. Read-only metadata.
 */
static struct hda_endpoint *hda_endpoint_for(struct hda_module_card *card,
                                             uint32_t device,enum audio_direction direction)
{
    if(!card)return NULL;
    if(direction==AUDIO_PLAYBACK)
        return device<card->playback_count?&card->playback[device]:NULL;
    if(direction==AUDIO_CAPTURE){
        if(!device)device=card->capture_source;
        return device<card->capture_count?&card->capture[device]:NULL;
    }
    return NULL;
}

/** @brief Intersect converter capabilities with implemented H4 formats/geometry.
 * @param opaque Borrowed module card.
 * @param device Logical PCM device.
 * @param direction Playback/capture selector.
 * @param caps Receives actual codec constraints.
 * @return 0 or negative errno; no hardware changes or ownership transfer.
 */
static int hda_card_format_caps(void *opaque,uint32_t device,enum audio_direction direction,
                                 struct audio_format_caps *caps)
{
    struct hda_module_card *card=opaque;
    struct hda_endpoint *ep=hda_endpoint_for(card,device,direction);
    if(!ep || !caps)return -ENODEV;
    struct hda_controller *c=&card->owner->hw;
    uint32_t isd=(c->gcap>>8)&15u,osd=(c->gcap>>12)&15u,bsd=(c->gcap>>3)&31u;
    if(!(direction==AUDIO_PLAYBACK?osd+bsd:isd+bsd))return -ENODEV;
    int ret=hda_pcm_format_caps(c,0,direction,caps);if(ret)return ret;
    uint32_t pcm=UINT32_MAX,formats=UINT32_MAX;
    for(uint32_t n=0;n<ep->group.member_count;n++){
        const struct hda_codec_node *node=&card->codec.nodes[ep->group.members[n].converter_nid];
        pcm&=node->pcm_rates;formats&=node->stream_formats;
    }
    caps->pcm.formats=0;caps->pcm.rates=0;
    memset(caps->format_bits,0,sizeof caps->format_bits);
    memset(caps->subformats,0,sizeof caps->subformats);
    if(formats&1u){
        if(pcm&(1u<<17)){
            caps->pcm.formats|=AUDIO_FORMAT_S16_LE;caps->format_bits[0]=AUDIO_FORMAT_S16_LE;
            caps->subformats[0]=1u<<AUDIO_SUBFORMAT_STD;
        }
        uint32_t sub=0;
        if(pcm&(1u<<18))sub|=1u<<AUDIO_SUBFORMAT_MSBITS_20;
        if(pcm&(1u<<19))sub|=1u<<AUDIO_SUBFORMAT_MSBITS_24;
        if(pcm&(1u<<20))sub|=(1u<<AUDIO_SUBFORMAT_MSBITS_MAX)|(1u<<AUDIO_SUBFORMAT_STD);
        if(sub){caps->pcm.formats|=AUDIO_FORMAT_S32_LE;caps->format_bits[1]=AUDIO_FORMAT_S32_LE;caps->subformats[1]=sub;}
    }
    if(pcm&(1u<<5))caps->pcm.rates|=AUDIO_RATE_44100;
    if(pcm&(1u<<6))caps->pcm.rates|=AUDIO_RATE_48000;
    caps->pcm.channels_min=caps->pcm.channels_max=ep->group.total_channels;
    caps->pcm.buffer_bytes_max=1024u*1024u;
    caps->pcm.period_bytes_max=caps->pcm.buffer_bytes_max/2u;
    return caps->pcm.formats && caps->pcm.rates && ep->group.total_channels?0:-ENODEV;
}

/** @brief Return the compatibility prefix of the real endpoint capabilities.
 * @param opaque Live card.
 * @param device Logical PCM number.
 * @param direction Playback/capture.
 * @param caps Receives the prefix.
 * @return 0 or errno. Task metadata query, no ownership transfer.
 */
static int hda_card_caps(void *opaque,uint32_t device,enum audio_direction direction,
                          struct audio_caps *caps)
{
    if(!caps)return -EINVAL;
    struct audio_format_caps ext;int ret=hda_card_format_caps(opaque,device,direction,&ext);
    if(!ret)*caps=ext.pcm;return ret;
}

/** @brief Own one card operation without waiting on another task's codec verbs.
 * @param card Borrowed card retained by the core callback reference.
 * @return True with ownership, false on contention or NULL.
 * @par Context Task callbacks; no spinlock or IRQ state change.
 */
static bool hda_card_enter(struct hda_module_card *card)
{
    uint32_t old=0;
    return card && __atomic_compare_exchange_n(&card->operation_busy,&old,1,false,
                                               __ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
}

/** @brief Release the card transaction after its stream and route changes.
 * @param card Card whose operation gate the caller owns.
 * @return None; release publishes the completed metadata transaction.
 */
static void hda_card_leave(struct hda_module_card *card)
{ __atomic_store_n(&card->operation_busy,0,__ATOMIC_RELEASE); }

/** @brief Locate the actual output amplifier on a resolved endpoint path.
 * @param card Borrowed codec metadata.
 * @param ep Resolved single-member analog endpoint.
 * @return Real amplifier NID or zero; bounded metadata lookup with no verbs.
 */
static uint8_t hda_card_output_amp(struct hda_module_card *card,struct hda_endpoint *ep)
{
    if(!ep || ep->group.member_count!=1)return 0;
    struct hda_route *r=&ep->group.members[0];
    for(unsigned n=0;n<r->path_count && n<HDA_ROUTE_MAX_NODES;n++){
        uint8_t nid=r->path[n];
        if(card->codec.nodes[nid].present &&
           (card->codec.nodes[nid].widget_caps&HDA_WCAP_OUT_AMP))return nid;
    }
    return 0;
}

/** @brief Read or write both implemented output amplifier channels.
 * @param card Live card under its operation gate.
 * @param nid Actual output amplifier NID.
 * @param values Borrowed two-channel words, output for read and input for write.
 * @param write Select write/readback instead of read.
 * @return Zero or verb errno; caller owns rollback of any partial write.
 * @par Context Waitable task; a nonwaiting codec gate spans no spinlock.
 */
static int hda_card_amp(struct hda_module_card *card,uint8_t nid,uint16_t values[2],bool write)
{
    if(!nid)return -ENODEV;
    struct hda_codec_node *node=&card->codec.nodes[nid];
    int ret=hda_codec_control_gate_enter(&card->codec);if(ret)return ret;
    for(unsigned n=0;n<((node->widget_caps&HDA_WCAP_STEREO)?2u:1u);n++){
        ret=write?hda_codec_control_write_amp(&card->codec,nid,false,0,n!=0,
                                              values[n],node->amp_output_caps):
                  hda_codec_control_read_amp(&card->codec,nid,false,0,n!=0,&values[n]);
        if(ret)break;
    }
    if(!write && !(node->widget_caps&HDA_WCAP_STEREO))values[1]=values[0];
    hda_codec_control_gate_leave(&card->codec);return ret;
}

/** @brief Detect converter ownership or a failed release that still needs retry.
 * @param group Borrowed route group.
 * @return True if any member retains stream binding metadata; no hardware access.
 */
static bool hda_card_group_bound(struct hda_route_group *group)
{
    if(!group)return false;
    for(unsigned n=0;n<group->member_count;n++)
        if(group->members[n].stream_bound || group->members[n].stream_bind_dirty)return true;
    return false;
}

/** @brief Bind actual converters then publish their complete stream ownership.
 * @param group Active route group borrowed from the card.
 * @param tag Existing SD tag or zero for release.
 * @param format Existing complete SD format, zero for release.
 * @return Zero after hardware verification, or errno with dirty leases retained.
 * @par Context Waitable task while card-owned SD RUN is clear.
 */
static int hda_card_group_bind(struct hda_route_group *group,uint8_t tag,uint16_t format)
{
    int ret=hda_route_group_stream_bind(group,tag,format);if(ret)return ret;
    for(unsigned n=0;n<group->member_count;n++){
        group->members[n].stream_bound=tag!=0;
        group->members[n].stream_tag=tag;group->members[n].stream_format=format;
    }
    return 0;
}

/** @brief Fail closed if rollback cannot prove the SD and codec topology safe.
 * @param card Live card under its operation gate.
 * @param stream Optional affected SD lease retained for later teardown.
 * @param error Negative failure recorded for diagnostics.
 * @return None; hide all cards on this BDF without freeing live DMA or metadata.
 * @par Context Waitable task outside core locks. STOP failure retains ownership.
 */
static void hda_card_policy_fault(struct hda_module_card *card,struct hda_stream *stream,int error)
{
    if(stream){
        struct audio_hw_stream hw={.id=stream->id,.driver=stream};
        (void)hda_pcm_trigger(&card->owner->hw,&hw,AUDIO_STOP);
    }
    card->controls.last_error=error;
    (void)kernel_api->audio_request_disconnect(card->id);
}

/** @brief Switch complete speaker/headphone groups while retaining the PCM lease.
 * @param opaque Borrowed live card under its operation and controls state gates.
 * @param enabled Requested auto-mute policy.
 * @param headphone_present Latest deduplicated hardware presence sample.
 * @return Zero after readback/resume, EBUSY for an independently leased headphone,
 * or original errno after rollback. Unprovable rollback disconnects the BDF.
 * @par Context Waitable task with IRQ enabled and no execution/core spinlock.
 * @par Ownership DMA, BDL, generation and SD tag remain owned by the original
 * speaker lease. RUN is cleared before any gain, route or converter mutation.
 */
static int hda_card_apply_policy(void *opaque,bool enabled,bool headphone_present)
{
    struct hda_module_card *card=opaque;
    struct hda_endpoint *old=card->policy_selected;
    struct hda_endpoint *target=enabled && headphone_present?card->policy_headphone:card->policy_speaker;
    if(!old || !target)return -ENODEV;
    if(card->policy_headphone->lease)return -EBUSY;
    if(old==target)return 0;
    if(target->group.active || hda_card_group_bound(&target->group))return -EBUSY;
    if(old->group.rollback_failed || target->group.rollback_failed)return -EIO;
    struct hda_stream *s=card->policy_speaker->lease;
    if(s && s->group && s->group!=&old->group)return -EBUSY;
    uint8_t old_nid=hda_card_output_amp(card,old),new_nid=hda_card_output_amp(card,target);
    uint16_t gains[2],muted[2];
    int ret=hda_card_amp(card,old_nid,gains,false);if(ret)return ret;
    bool running=s && s->state==HDA_STREAM_RUNNING;
    bool old_active=old->group.active,old_bound=hda_card_group_bound(&old->group);
    bool old_disable_attempted=false;
    if(old_bound && !s)return -EBUSY;
    uint8_t tag=old_bound?old->group.members[0].stream_tag:0;
    uint16_t format=old_bound?old->group.members[0].stream_format:0;
    struct audio_hw_stream hw={.id=s?s->id:0,.driver=s};
    if(running){ret=hda_pcm_trigger(&card->owner->hw,&hw,AUDIO_PAUSE);if(ret)return ret;}
    /* Mute during the silent topology window; restore user gain/mute on target.
     * A shared DAC is unmuted only after the old pin path has been disabled. */
    muted[0]=gains[0]|0x80u;muted[1]=gains[1]|0x80u;
    ret=hda_card_amp(card,old_nid,muted,true);if(ret)goto restore;
    if(old_bound){ret=hda_card_group_bind(&old->group,0,0);if(ret)goto restore;}
    if(old->group.active){
        old_disable_attempted=true;
        ret=hda_apply_route_group(&card->codec,&old->group,false);if(ret)goto restore;
    }
    ret=hda_apply_route_group(&card->codec,&target->group,true);if(ret)goto restore;
    ret=hda_card_amp(card,new_nid,gains,true);if(ret)goto restore;
    if(old_bound){ret=hda_card_group_bind(&target->group,tag,format);if(ret)goto restore;}
    if(s)s->group=&target->group;
    if(running){
        ret=hda_pcm_trigger(&card->owner->hw,&hw,AUDIO_UNPAUSE);
        if(ret){hda_card_policy_fault(card,s,ret);return ret;}
    }
    card->policy_selected=target;card->controls.playback_amp_nid=new_nid;
    card->controls.policy_mute=0;return 0;
restore:
    {
        int original=ret,rollback=0,next;
        if(hda_card_group_bound(&target->group)){
            next=hda_card_group_bind(&target->group,0,0);if(next)rollback=next;
        }
        if(!rollback && target->group.active){
            next=hda_apply_route_group(&card->codec,&target->group,false);if(next)rollback=next;
        }
        /* active after a failed restore means a retained, possibly partial
         * snapshot. Complete that restore before taking a fresh original path. */
        if(!rollback && old_disable_attempted && old->group.active){
            next=hda_apply_route_group(&card->codec,&old->group,false);if(next)rollback=next;
        }
        if(!rollback && old_active && !old->group.active){
            next=hda_apply_route_group(&card->codec,&old->group,true);if(next)rollback=next;
        }
        if(!rollback){next=hda_card_amp(card,old_nid,gains,true);if(next)rollback=next;}
        if(!rollback && old_bound){
            /* Failed releases may still retain a dirty tag; clear before rebind. */
            if(hda_card_group_bound(&old->group)){
                next=hda_card_group_bind(&old->group,0,0);if(next)rollback=next;
            }
            if(!rollback){next=hda_card_group_bind(&old->group,tag,format);if(next)rollback=next;}
        }
        if(s)s->group=&old->group;
        if(!rollback && running){next=hda_pcm_trigger(&card->owner->hw,&hw,AUDIO_UNPAUSE);if(next)rollback=next;}
        if(rollback){
            /* Keep whichever group still owns converters reachable by close;
             * all groups also remain card-owned for teardown retry. */
            if(s)s->group=target->group.active?&target->group:
                          old->group.active?&old->group:NULL;
            hda_card_policy_fault(card,s,rollback);
        }
        else card->controls.last_error=original;
        return original;
    }
}

/** @brief Lease a free SD of the correct physical input/output direction.
 * @param opaque Borrowed card.
 * @param device Logical PCM number, independent of physical SD index.
 * @param direction Playback/capture.
 * @param out Receives the H4 hardware lease.
 * @return 0, EBUSY, or negative errno. Task-only; leases shared controller slots.
 */
static int hda_card_open_locked(void *opaque,uint32_t device,enum audio_direction direction,
                          struct audio_hw_stream *out)
{
    struct hda_module_card *card=opaque;
    struct hda_endpoint *ep=hda_endpoint_for(card,device,direction);
    struct audio_caps caps;int ret=hda_card_caps(opaque,device,direction,&caps);
    if(ret)return ret;if(!out)return -EINVAL;if(ep->lease)return -EBUSY;
    if(ep==card->policy_headphone && card->controls.auto_mute)return -EBUSY;
    struct hda_controller *c=&card->owner->hw;
    uint32_t inputs=(c->gcap>>8)&15u,outputs=(c->gcap>>12)&15u;
    for(uint32_t i=0;i<c->stream_count;i++){
        bool usable=direction==AUDIO_CAPTURE?i<inputs:i>=inputs && i<inputs+outputs;
        usable|=i>=inputs+outputs;
        if(!usable || c->streams[i].state!=HDA_STREAM_FREE)continue;
        ret=hda_pcm_open(c,i,direction,out);if(ret)return ret;
        ep->lease=out->driver;ep->lease->card_id=card->id;
        return 0;
    }
    return -EBUSY;
}

/** @brief Serialize endpoint lease admission with task-context jack rerouting.
 * @param opaque Borrowed live card.
 * @param device Logical PCM device.
 * @param direction Playback/capture direction.
 * @param out Receives the owned hardware lease.
 * @return Zero, EBUSY on gate/endpoint contention, or the open errno.
 */
static int hda_card_open(void *opaque,uint32_t device,enum audio_direction direction,
                          struct audio_hw_stream *out)
{
    struct hda_module_card *card=opaque;
    if(!hda_card_enter(card))return -EBUSY;
    int ret=hda_card_open_locked(opaque,device,direction,out);hda_card_leave(card);return ret;
}

/** @brief Find an endpoint only when its hardware lease belongs to this card.
 * @param card Live card metadata.
 * @param stream Borrowed generation-tagged stream.
 * @return Matching endpoint or NULL. No lock or ownership transfer.
 */
static struct hda_endpoint *hda_endpoint_lease(struct hda_module_card *card,
                                               struct audio_hw_stream *stream)
{
    if(!card || !stream || !stream->driver || !stream->id)return NULL;
    for(uint32_t dir=0;dir<2;dir++){
        struct hda_endpoint *eps=dir?card->capture:card->playback;
        uint32_t count=dir?card->capture_count:card->playback_count;
        for(uint32_t n=0;n<count;n++)
            if(eps[n].lease==stream->driver && eps[n].lease->id==stream->id)return &eps[n];
    }
    return NULL;
}

/** @brief Activate one stopped analog endpoint and bind the exact stream format.
 * @param opaque Live card.
 * @param stream Owned open SD lease.
 * @param params Borrowed selected PCM format.
 * @param dma Borrowed core PCM buffer, never freed here.
 * @return 0 or errno. Waitable task context; failed route snapshots stay retryable.
 * Mixer values are restored after converter binding, before START. A failed
 * amplifier restore stops the prepared lease and keeps its DMA for close/retry.
 */
static int hda_card_prepare_format_locked(void *opaque,struct audio_hw_stream *stream,
                                    const struct audio_params_ext *params,const struct audio_dma *dma)
{
    struct hda_module_card *card=opaque;
    struct hda_endpoint *ep=hda_endpoint_lease(card,stream);
    if(!ep || !params)return -EINVAL;
    if(ep->lease->state!=HDA_STREAM_OPEN && ep->lease->state!=HDA_STREAM_STOPPED)return -EBUSY;
    uint32_t device=(uint32_t)(ep-(ep->is_capture?card->capture:card->playback));
    /* Source selection is frozen while a capture endpoint has a hardware lease. */
    struct audio_format_caps caps;int ret=hda_card_format_caps(card,device,
        ep->is_capture?AUDIO_CAPTURE:AUDIO_PLAYBACK,&caps);
    if(ret)return ret;
    uint64_t rate=params->pcm.rate==44100?AUDIO_RATE_44100:
                  params->pcm.rate==48000?AUDIO_RATE_48000:0;
    if(!(caps.pcm.formats&params->format) || !(caps.pcm.rates&rate) ||
        params->pcm.channels!=ep->group.total_channels)return -EINVAL;
    struct hda_route_group *group=ep==card->policy_speaker?&card->policy_selected->group:&ep->group;
    if(ep->lease->group && hda_card_group_bound(ep->lease->group)){
        ret=hda_card_group_bind(ep->lease->group,0,0);if(ret)return ret;
    }
    if(hda_card_group_bound(group)){ret=hda_card_group_bind(group,0,0);if(ret)return ret;}
    uint8_t nid=0;
    /* The published mixer also exists on codecs without auto-mute. Only
     * restore its amplifier when that real NID belongs to this output group. */
    if(!ep->is_capture && card->controls_ready && card->controls.playback_amp_nid){
        for(unsigned m=0;m<group->member_count;m++){
            struct hda_route *route=&group->members[m];
            for(unsigned n=0;n<route->path_count && n<HDA_ROUTE_MAX_NODES;n++)
                if(route->path[n]==card->controls.playback_amp_nid)
                    nid=card->controls.playback_amp_nid;
        }
    }
    uint16_t original_gain[2]={0,0};
    if(nid){ret=hda_card_amp(card,nid,original_gain,false);if(ret)return ret;}
    bool activated=!group->active;
    if(activated){
        ret=ep->is_capture?hda_apply_route(&card->codec,&ep->capture,true):
                           hda_apply_route_group(&card->codec,group,true);
        if(ep->is_capture)group->active=ep->capture.active;
        if(ret){hda_module_error("route apply failed",&card->owner->hw.dev,card->codec.cad,ret);return ret;}
    }
    ep->lease->group=group;
    ret=hda_pcm_prepare_format(&card->owner->hw,stream,params,dma);
    if(ret){hda_module_error("SD prepare failed",&card->owner->hw.dev,card->codec.cad,ret);return ret;}
    /* Converter format binding may recreate the codec's output voice. Restore
     * the user's gain/mute after that operation, while RUN is still clear. */
    if(nid){
        uint16_t gain[2]={card->controls.playback_gain[0],card->controls.playback_gain[1]};
        if(card->controls.playback_mute){gain[0]|=0x80u;gain[1]|=0x80u;}
        ret=hda_card_amp(card,nid,gain,true);
        if(ret){
            int rollback=hda_pcm_trigger(&card->owner->hw,stream,AUDIO_STOP);
            if(!rollback)rollback=hda_card_amp(card,nid,original_gain,true);
            if(rollback)hda_card_policy_fault(card,ep->lease,rollback);
            return ret;
        }
    }
    return 0;
}

/** @brief Prepare PCM under the same nonwaiting gate as route and policy writes.
 * @param opaque Borrowed card.
 * @param stream Owned open hardware lease.
 * @param params Borrowed selected format.
 * @param dma Borrowed core-owned DMA buffer.
 * @return Zero, EBUSY on concurrent policy, or the preparation errno.
 * @par Context Waitable task; no execution/core spinlock spans codec verbs.
 */
static int hda_card_prepare_format(void *opaque,struct audio_hw_stream *stream,
                                    const struct audio_params_ext *params,const struct audio_dma *dma)
{
    struct hda_module_card *card=opaque;
    if(!hda_card_enter(card))return -EBUSY;
    int ret=hda_card_prepare_format_locked(opaque,stream,params,dma);hda_card_leave(card);return ret;
}

/** @brief Prepare the explicit legacy S16 path.
 * @param opaque Live card.
 * @param stream Open lease.
 * @param params Borrowed legacy parameters.
 * @param dma Borrowed core DMA.
 * @return 0 or errno. Waitable task context; unsupported precision is rejected.
 */
static int hda_card_prepare(void *opaque,struct audio_hw_stream *stream,
                             const struct audio_params *params,const struct audio_dma *dma)
{
    if(!params || params->sample_bits!=16u)return -EINVAL;
    struct audio_params_ext ext={.pcm=*params,.format=AUDIO_FORMAT_S16_LE,
        .subformat=AUDIO_SUBFORMAT_STD,.significant_bits=16u};
    return hda_card_prepare_format(opaque,stream,&ext,dma);
}

/** @brief Trigger only an SD lease held by this card.
 * @param opaque Live card.
 * @param stream Borrowed hardware lease.
 * @param trigger START/STOP/PAUSE/UNPAUSE.
 * @return 0 or errno. Bounded nonblocking callback, no codec verbs.
 */
static int hda_card_trigger(void *opaque,struct audio_hw_stream *stream,enum audio_trigger trigger)
{
    struct hda_module_card *card=opaque;
    if(!hda_card_enter(card))return -EBUSY;
    int ret=hda_endpoint_lease(card,stream)?hda_pcm_trigger(&card->owner->hw,stream,trigger):-EINVAL;
    hda_card_leave(card);return ret;
}

/** @brief Read completed frames from this card's actual SD lease.
 * @param opaque Live card.
 * @param stream Borrowed hardware lease.
 * @param frames Receives monotonic frame position.
 * @return 0 or errno. IRQ-safe, no wait or ownership change.
 */
static int hda_card_pointer(void *opaque,struct audio_hw_stream *stream,uint64_t *frames)
{
    struct hda_module_card *card=opaque;
    return hda_endpoint_lease(card,stream)?hda_pcm_pointer(&card->owner->hw,stream,frames):-EINVAL;
}

/** @brief Close a stopped lease without discarding a failed converter cleanup.
 * @param opaque Live card.
 * @param stream Stopped hardware lease.
 * @return None. Task context; a failed close remains in endpoint metadata for fini retry.
 */
static void hda_card_close(void *opaque,struct audio_hw_stream *stream)
{
    struct hda_module_card *card=opaque;
    if(!hda_card_enter(card))return;
    struct hda_endpoint *ep=hda_endpoint_lease(card,stream);
    if(ep){hda_pcm_close(&card->owner->hw,stream);if(!stream->driver)ep->lease=NULL;}
    hda_card_leave(card);
}

/** @brief Return only controls backed by this codec's discovered hardware.
 * @param opaque Live card.
 * @return Frozen registration count. No wait or ownership transfer.
 */
static uint32_t hda_card_control_count(void *opaque)
{ return opaque?((struct hda_module_card*)opaque)->control_count:0; }

/** @brief Describe a discovered mixer control or actual capture route selector.
 * @param opaque Live card.
 * @param index Registration-order control index.
 * @param info Receives public module control metadata.
 * @return 0 or errno. Task metadata query, no hardware changes.
 */
static int hda_card_control_info(void *opaque,uint32_t index,struct audio_control_info *info)
{
    struct hda_module_card *card=opaque;
    if(!card || !info || index>=card->control_count)return -EINVAL;
    uint32_t id=card->control_ids[index];int ret=0;
    if(id==HDA_CONTROL_CAPTURE_SOURCE){
        memset(info,0,sizeof *info);info->type=AUDIO_CONTROL_ENUMERATED;info->count=1;
        info->access=AUDIO_CONTROL_READ|(card->capture_count>1?AUDIO_CONTROL_WRITE:0);
        info->max=(int64_t)card->capture_count-1;info->step=1;info->items=card->capture_count;
        const char name[]="Capture Source";
        for(unsigned n=0;n<sizeof name;n++)info->name[n]=name[n];
        for(unsigned n=0;n<card->capture_count;n++){
            unsigned kind=(card->codec.nodes[card->capture[n].capture.pin_nid].pin_default>>20)&15u;
            const char *label=kind==10u?"Mic Pin 00":"Line Pin 00";
            unsigned pos=0;while(label[pos]){info->item_names[n][pos]=label[pos];pos++;}
            uint8_t pin=card->capture[n].capture.pin_nid;
            info->item_names[n][pos-2]="0123456789abcdef"[pin>>4];
            info->item_names[n][pos-1]="0123456789abcdef"[pin&15u];
        }
    }else ret=hda_controls_info(&card->controls,id,info);
    if(!ret)info->id=index;return ret;
}

/** @brief Read an actual gain/mute or selected physical capture endpoint.
 * @param opaque Live card.
 * @param index Frozen control index.
 * @param value Receives scalar/stereo values.
 * @return 0 or errno. Waitable task context for amplifier readback.
 */
static int hda_card_control_read_locked(void *opaque,uint32_t index,struct audio_control_value *value)
{
    struct hda_module_card *card=opaque;
    if(!card || !value || index>=card->control_count)return -EINVAL;
    if(card->control_ids[index]==HDA_CONTROL_CAPTURE_SOURCE){
        memset(value,0,sizeof *value);value->values[0]=card->capture_source;return 0;
    }
    return hda_controls_read(&card->controls,card->control_ids[index],value);
}

/** @brief Read mixer state without racing the selected physical route.
 * @param opaque Live pinned card.
 * @param index Frozen published element index.
 * @param value Receives real gain/mute or scalar state.
 * @return Zero, EBUSY on transaction contention, or the read errno.
 * @par Context Waitable task outside core locks.
 */
static int hda_card_control_read(void *opaque,uint32_t index,struct audio_control_value *value)
{
    struct hda_module_card *card=opaque;if(!hda_card_enter(card))return -EBUSY;
    int ret=hda_card_control_read_locked(opaque,index,value);hda_card_leave(card);return ret;
}

/** @brief Write real mixer state or select a capture route while PCM is closed.
 * @param opaque Live card.
 * @param index Frozen control index.
 * @param value Borrowed validated values.
 * @return 0 or errno. Waitable task context; open capture prohibits topology changes.
 */
static int hda_card_control_write_locked(void *opaque,uint32_t index,const struct audio_control_value *value)
{
    struct hda_module_card *card=opaque;
    if(!card || !value || index>=card->control_count)return -EINVAL;
    if(card->control_ids[index]!=HDA_CONTROL_CAPTURE_SOURCE)
        return hda_controls_write(&card->controls,card->control_ids[index],value);
    if(card->capture_count<2)return -EACCES;
    if(value->values[0]<0 || (uint64_t)value->values[0]>=card->capture_count)return -ERANGE;
    for(unsigned n=0;n<card->capture_count;n++)if(card->capture[n].lease)return -EBUSY;
    card->capture_source=(uint8_t)value->values[0];
    kernel_api->audio_control_changed(card->id,index);return 0;
}

/** @brief Serialize mixer/source/policy writes with PCM lease and trigger changes.
 * @param opaque Live pinned card.
 * @param index Frozen published element index.
 * @param value Borrowed requested control values.
 * @return Zero, EBUSY on contention, or original transaction errno.
 * @par Context Waitable task; gate is nonwaiting and no spinlock spans verbs.
 */
static int hda_card_control_write(void *opaque,uint32_t index,const struct audio_control_value *value)
{
    struct hda_module_card *card=opaque;if(!hda_card_enter(card))return -EBUSY;
    int ret=hda_card_control_write_locked(opaque,index,value);hda_card_leave(card);return ret;
}

/** @brief Progress the published headphone sensor in a pinned task phase.
 * @param opaque Live card retained by the audio core callback reference.
 * @param budget Shared task transport/event/sense work limit.
 * @return Actual bounded work, zero for a codec without a presence sensor.
 * @par Context Manager task wait phase with IRQ enabled and execution released.
 * @par Ownership and locks/IRQ No core lock spans verbs; nonwaiting card and
 * control gates preserve pending samples/policy on contention. IRQ remains enabled.
 */
static uint32_t hda_card_task_service(void *opaque,uint32_t budget)
{
    struct hda_module_card *card=opaque;
    if(!card || !card->controls_ready || !hda_card_enter(card))return 0;
    uint32_t work=hda_controls_task_service(&card->controls,budget);
    hda_card_leave(card);return work;
}

static const struct audio_card_ops hda_card_ops={.version=AUDIO_CARD_OPS_VERSION,
    .size=sizeof(struct audio_card_ops),.pcm_caps=hda_card_caps,.open=hda_card_open,
    .prepare=hda_card_prepare,.trigger=hda_card_trigger,.pointer=hda_card_pointer,
    .close=hda_card_close,.control_count=hda_card_control_count,.control_info=hda_card_control_info,
    .control_read=hda_card_control_read,.control_write=hda_card_control_write,
    .pcm_format_caps=hda_card_format_caps,.prepare_format=hda_card_prepare_format,
    .task_service=hda_card_task_service};

/** @brief Service the shared controller once per card with a bounded budget.
 * @param opaque Borrowed card, pinned by the core service dispatcher.
 * @param budget Maximum completions.
 * @return Actual bounded work. No wait, allocation or unregister callback.
 */
static uint32_t hda_card_service(void *opaque,uint32_t budget)
{
    struct hda_module_card *card=opaque;
    if(!budget)return 0;
    uint32_t work=0,token;
    if(!hda_controller_take_disconnect_request(&card->owner->hw,&token)){
        (void)kernel_api->audio_request_disconnect(token);work=1;
    }
    if(work<budget)work+=hda_controller_service_budget(&card->owner->hw,budget-work);
    return work;
}

/** @brief Discover all distinct analog associations and capture pin paths.
 * @param card Probed codec metadata, retained even after partial failure.
 * @return 0 if any endpoint is usable, or errno. Task init; exact group allocations owned here.
 */
static int hda_card_routes(struct hda_module_card *card)
{
    uint16_t seen=0;
    for(unsigned pin=1;pin<HDA_CODEC_MAX_NIDS;pin++){
        struct hda_codec_node *node=&card->codec.nodes[pin];
        if(!node->present || node->type!=HDA_WIDGET_PIN || (node->widget_caps&HDA_WCAP_DIGITAL) ||
            ((node->pin_default>>30)&3u)==1u)continue;
        unsigned association=(node->pin_default>>4)&15u;
        if((node->pin_caps&HDA_PINCAP_OUTPUT) && association &&
            (association==15 || !(seen&(1u<<association))) && card->playback_count<HDA_MODULE_DEVICES){
            struct hda_endpoint *ep=&card->playback[card->playback_count];
            int ret=hda_find_route_group(&card->codec,(uint8_t)pin,&ep->group);
            if(!ret){card->playback_count++;seen|=(uint16_t)(1u<<association);}
            else if(ret==-ENOMEM)return ret;
        }
        if((node->pin_caps&HDA_PINCAP_INPUT) && card->capture_count<HDA_MODULE_DEVICES){
            struct hda_endpoint *ep=&card->capture[card->capture_count];
            int ret=hda_find_route(&card->codec,(uint8_t)pin,HDA_ROUTE_CAPTURE,&ep->capture);
            if(ret){if(ret==-ENOMEM)return ret;continue;}
            uint8_t channels=16;
            for(unsigned n=0;n<ep->capture.path_count;n++){
                uint32_t wc=card->codec.nodes[ep->capture.path[n]].widget_caps;
                uint8_t capacity=(uint8_t)((((wc>>13)&7u)<<1)|(wc&1u))+1u;
                if(capacity<channels)channels=capacity;
            }
            ep->is_capture=1;ep->capture.channel_count=channels;
            ep->group.codec=&card->codec;ep->group.members=&ep->capture;
            ep->group.member_count=1;ep->group.total_channels=channels;
            card->capture_count++;
        }
    }
    return card->playback_count || card->capture_count?0:-ENODEV;
}

/** @brief Freeze the real gain/mute, capture and headphone registration indices.
 * @param card Live probed codec with resolved groups, not yet published.
 * @return None. Unavailable hardware controls are omitted from the frozen set.
 * @par Context Task init; amplifier readback/unsolicited setup may wait.
 * @par Ownership and locks/IRQ Routes remain card-owned. Auto-mute is exposed
 * only for compatible single-member stereo groups with real mute amplifiers.
 */
static void hda_card_init_controls(struct hda_module_card *card)
{
    struct hda_route *speaker=NULL,*headphone=NULL;
    struct hda_endpoint *speaker_ep=NULL,*headphone_ep=NULL;
    for(unsigned ep=0;ep<card->playback_count;ep++){
        struct hda_route_group *group=&card->playback[ep].group;
        for(unsigned n=0;n<group->member_count;n++){
            struct hda_route *route=&group->members[n];
            struct hda_codec_node *pin=&card->codec.nodes[route->pin_nid];
            unsigned kind=(pin->pin_default>>20)&15u;
            if(!speaker && (kind==0u || kind==1u)){speaker=route;speaker_ep=&card->playback[ep];}
            if(!headphone && kind==2u && (pin->pin_caps&HDA_PINCAP_PRESENCE)){
                headphone=route;headphone_ep=&card->playback[ep];
            }
        }
    }
    if(!speaker && card->playback_count)speaker=&card->playback[0].group.members[0];
    if(speaker && !hda_controls_init(&card->controls,&card->codec,speaker,headphone,
                                    speaker->pin_nid,headphone?headphone->pin_nid:0,
                                    NULL,0,false)){
        card->controls_ready=1;
        struct audio_control_info info;
        if(!hda_controls_info(&card->controls,HDA_CONTROL_PLAYBACK_VOLUME,&info))
            card->control_ids[card->control_count++]=HDA_CONTROL_PLAYBACK_VOLUME;
        if(!hda_controls_info(&card->controls,HDA_CONTROL_PLAYBACK_MUTE,&info) &&
            (info.access&AUDIO_CONTROL_WRITE))
            card->control_ids[card->control_count++]=HDA_CONTROL_PLAYBACK_MUTE;
    }
    if(card->capture_count)card->control_ids[card->control_count++]=HDA_CONTROL_CAPTURE_SOURCE;
    if(card->controls_ready && card->controls.headphone_pin)
        card->control_ids[card->control_count++]=HDA_CONTROL_HEADPHONE_JACK;
    if(card->controls_ready && speaker_ep && headphone_ep && speaker_ep!=headphone_ep &&
       speaker_ep->group.member_count==1 && headphone_ep->group.member_count==1 &&
       speaker_ep->group.total_channels==2 && headphone_ep->group.total_channels==2){
        uint8_t a=hda_card_output_amp(card,speaker_ep),b=hda_card_output_amp(card,headphone_ep);
        struct hda_codec_node *ac=&card->codec.nodes[speaker->converter_nid];
        struct hda_codec_node *bc=&card->codec.nodes[headphone->converter_nid];
        if(a && b && (card->codec.nodes[a].amp_output_caps&(1u<<31)) &&
           card->codec.nodes[a].amp_output_caps==card->codec.nodes[b].amp_output_caps &&
           !((card->codec.nodes[a].widget_caps^card->codec.nodes[b].widget_caps)&HDA_WCAP_STEREO) &&
           ac->pcm_rates==bc->pcm_rates && ac->stream_formats==bc->stream_formats){
            card->policy_speaker=card->policy_selected=speaker_ep;
            card->policy_headphone=headphone_ep;
            card->controls.apply_policy=hda_card_apply_policy;card->controls.policy_opaque=card;
            card->control_ids[card->control_count++]=HDA_CONTROL_AUTO_MUTE;
        }
    }
    for(unsigned id=0;id<5u;id++)card->controls.control_index[id]=UINT32_MAX;
    for(unsigned index=0;index<card->control_count;index++)
        card->controls.control_index[card->control_ids[index]]=index;
}

/** @brief Build a stable card ID from BDF and codec without addresses in logs.
 * @param card Probed module card.
 * @param identity Receives the complete core identity.
 * @return None. Pure metadata copy; core later owns its snapshot.
 */
static void hda_card_identity(struct hda_module_card *card,struct audio_card_identity *identity)
{
    struct reliefos_driver_pci_device *d=&card->owner->hw.dev;
    *identity=(struct audio_card_identity){.bus=d->bus,.slot=d->slot,.function=d->function,
        .codec=card->codec.cad,.vendor=d->vendor_id,.device=d->device_id,
        .codec_id=card->codec.vendor_id,.subsystem_id=card->codec.subsystem_id};
    const char *hex="0123456789abcdef",*prefix="HDA";
    for(unsigned n=0;n<3;n++)identity->id[n]=prefix[n];
    identity->id[3]=hex[d->bus>>4];identity->id[4]=hex[d->bus&15u];
    identity->id[5]=hex[d->slot>>4];identity->id[6]=hex[d->slot&15u];
    identity->id[7]=hex[d->function];identity->id[8]=hex[card->codec.cad];
    const char name[]="Intel HDA analog codec";
    for(unsigned n=0;n<sizeof name;n++)identity->name[n]=name[n];
}

/** @brief Restore and destroy one codec after all its PCM leases are stopped.
 * @param card Owned card metadata retained on any uncertain release.
 * @return 0 or first negative error. Task teardown; failed resources remain retryable.
 */
static int hda_card_destroy(struct hda_module_card *card)
{
    if(card->id){
        int ret=kernel_api->audio_unregister_card(card->id,1);
        if(ret && ret!=-ENODEV)return ret;
        card->id=0;
    }
    for(unsigned dir=0;dir<2;dir++){
        struct hda_endpoint *eps=dir?card->capture:card->playback;
        unsigned count=dir?card->capture_count:card->playback_count;
        for(unsigned n=0;n<count;n++){
            struct hda_endpoint *ep=&eps[n];
            if(ep->lease){
                struct audio_hw_stream hw={.id=ep->lease->id,.driver=ep->lease};
                int ret=hda_pcm_trigger(&card->owner->hw,&hw,AUDIO_STOP);if(ret)return ret;
                hda_pcm_close(&card->owner->hw,&hw);if(hw.driver)return -EBUSY;
                ep->lease=NULL;
            }
            if(hda_card_group_bound(&ep->group)){
                int ret=hda_card_group_bind(&ep->group,0,0);if(ret)return ret;
            }
            if(ep->group.active){
                int ret=dir?hda_apply_route(&card->codec,&ep->capture,false):
                            hda_apply_route_group(&card->codec,&ep->group,false);
                if(dir)ep->group.active=ep->capture.active;
                if(ret)return ret;
            }
            if(!dir && ep->group.members){int ret=hda_route_group_destroy(&ep->group);if(ret)return ret;}
        }
    }
    hda_controls_destroy(&card->controls);
    return card->codec.probed?hda_codec_destroy(&card->codec):0;
}

/** @brief Stop all cards/controllers and retain every unsafe controller for retry.
 * @return None. Task fini; report failure before manager may reclaim DMA/MMIO/code.
 * No controller is removed from the list until its complete cleanup succeeds.
 */
static void hda_module_fini(void)
{
    if(!kernel_api)return;
    int first_error=0;
    struct hda_module_controller **link=&controllers;
    while(*link){
        struct hda_module_controller *owner=*link;int ret=0;
        while(owner->cards){
            struct hda_module_card *card=owner->cards;
            ret=hda_card_destroy(card);if(ret)break;
            owner->cards=card->next;
            kernel_api->free_pages(card->phys,card->pages);
        }
        if(!ret && owner->hw.streams)ret=hda_stream_controller_destroy(&owner->hw);
        if(!ret && owner->hw.api)ret=hda_controller_destroy(&owner->hw);
        if(ret){if(!first_error)first_error=ret;link=&owner->next;continue;}
        *link=owner->next;kernel_api->free_pages(owner->phys,owner->pages);
    }
    if(first_error){kernel_api->console_write("[hda] unsafe teardown retained for retry\n");
        kernel_api->report_teardown_failure(first_error);return;}
    published_cards=0;kernel_api=NULL;
}

/** @brief Enumerate every HDA BDF and publish usable per-codec cards.
 * @param api Borrowed size/version-checked manager contract, live until safe fini.
 * @return 0 with at least one usable card, or errno. Task init; manager owns rollback.
 */
static int hda_module_init(const struct reliefos_driver_kernel_api *api)
{
    if(!api || api->abi_version!=RELIEFOS_DRIVER_ABI_VERSION ||
        api->struct_size<RELIEFOS_DRIVER_DISCONNECT_API_SIZE || !api->report_teardown_failure ||
        !api->audio_request_disconnect ||
        !api->pci_enumerate || !api->alloc_pages || !api->free_pages ||
        !api->audio_register_card || !api->audio_unregister_card || !api->audio_period_elapsed ||
        !api->audio_control_changed || !api->audio_set_service || !api->console_write)return -EINVAL;
    if(controllers)return -EBUSY;
    kernel_api=api;published_cards=0;unsigned controller_count=0;
    for(uint32_t index=0;index<65536u;index++){
        struct reliefos_driver_pci_device dev;int ret=api->pci_enumerate(index,&dev);
        if(ret==-ENODEV)break;if(ret)return ret;
        if(dev.class_code!=4u || dev.subclass!=3u)continue;
        if(controller_count++>=HDA_MODULE_CONTROLLERS)return -ENOSPC;
        uint64_t phys;uint32_t pages;
        struct hda_module_controller *owner=hda_module_alloc(sizeof *owner,&phys,&pages);
        if(!owner)return -ENOMEM;
        owner->phys=phys;owner->pages=pages;owner->next=controllers;controllers=owner;
        ret=hda_controller_init(&owner->hw,api,&dev);
        if(ret){hda_module_error("controller init failed",&dev,0,ret);return ret;}
        unsigned slots=((owner->hw.gcap>>8)&15u)+((owner->hw.gcap>>12)&15u)+((owner->hw.gcap>>3)&31u);
        if(!slots || slots>HDA_STREAM_MAX)return -EOPNOTSUPP;
        ret=hda_stream_controller_init(&owner->hw,owner->streams,slots);
        if(ret){hda_module_error("stream init failed",&dev,0,ret);return ret;}
        for(unsigned cad=0;cad<=HDA_CODEC_MAX_CAD;cad++){
            if(!(owner->hw.codec_mask&(1u<<cad)))continue;
            struct hda_module_card *card=hda_module_alloc(sizeof *card,&phys,&pages);
            if(!card)return -ENOMEM;
            card->owner=owner;card->phys=phys;card->pages=pages;
            card->next=owner->cards;owner->cards=card;
            ret=hda_codec_probe(&owner->hw,(uint8_t)cad,&card->codec);
            if(ret){hda_module_error("codec probe failed",&dev,(uint8_t)cad,ret);if(ret==-ENOMEM)return ret;continue;}
            hda_codec_dump(&card->codec);
            ret=hda_card_routes(card);
            if(ret){hda_module_error("routes unavailable",&dev,(uint8_t)cad,ret);if(ret==-ENOMEM)return ret;continue;}
            struct audio_caps caps;
            if(hda_card_caps(card,0,AUDIO_PLAYBACK,&caps) &&
                hda_card_caps(card,0,AUDIO_CAPTURE,&caps))continue;
            if(published_cards>=HDA_MODULE_CARDS)return -ENOSPC;
            hda_card_init_controls(card);
            struct audio_card_identity identity;hda_card_identity(card,&identity);
            struct audio_card_ops ops=hda_card_ops;
            if(!card->controls_ready || !card->controls.headphone_pin)ops.task_service=NULL;
            ret=api->audio_register_card(&identity,&ops,card,&card->id);if(ret)return ret;
            card->controls.card_id=card->id;
            if(!owner->hw.card_id)hda_controller_set_card_id(&owner->hw,card->id);
            ret=api->audio_set_service(card->id,hda_card_service,card);if(ret)return ret;
            published_cards++;
            api->console_write("[hda] registered usable codec card\n");
        }
    }
    return published_cards?0:-ENODEV;
}

const struct reliefos_driver_module reliefos_driver_module={
    .magic=RELIEFOS_DRIVER_MODULE_MAGIC,.abi_version=RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size=sizeof(struct reliefos_driver_module),.kind=RELIEFOS_DRIVER_KIND_AUDIO,
    .name="Intel HDA",.version=1,.init=hda_module_init,.fini=hda_module_fini};

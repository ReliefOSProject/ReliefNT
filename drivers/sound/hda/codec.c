#include "hda.h"

#include <linux/errno.h>

#define HDA_CODEC_PAGE_BYTES 4096u
#define HDA_CODEC_CONNECTION_BYTES (HDA_CODEC_MAX_NIDS * HDA_CODEC_MAX_CONNECTIONS)
#define HDA_ROOT_NID 0u

#define HDA_VERB_GET_PARAMETER 0xf00u
#define HDA_VERB_GET_CONNECTION_LIST 0xf02u
#define HDA_VERB_GET_POWER_STATE 0xf05u
#define HDA_VERB_GET_CONNECTION_SELECT 0xf01u
#define HDA_VERB_GET_PIN_CONTROL 0xf07u
#define HDA_VERB_GET_EAPD 0xf0cu
#define HDA_VERB_GET_AMP 0x0bu
#define HDA_VERB_GET_CONFIG_DEFAULT 0xf1cu
#define HDA_VERB_GET_IMPLEMENTATION_ID 0xf20u
#define HDA_VERB_SET_POWER_STATE 0x705u
#define HDA_VERB_SET_CONNECTION_SELECT 0x701u
#define HDA_VERB_SET_PIN_CONTROL 0x707u
#define HDA_VERB_SET_EAPD 0x70cu
#define HDA_VERB_SET_AMP 0x03u
#define HDA_VERB_GET_STREAM_CHANNEL 0xf06u
#define HDA_VERB_SET_STREAM_CHANNEL 0x706u
#define HDA_VERB_GET_CONVERTER_FORMAT 0xa00u
#define HDA_VERB_SET_CONVERTER_FORMAT 0x02u

#define HDA_PARAM_VENDOR_ID 0x00u
#define HDA_PARAM_REVISION_ID 0x02u
#define HDA_PARAM_NODE_COUNT 0x04u
#define HDA_PARAM_FUNCTION_TYPE 0x05u
#define HDA_PARAM_AFG_CAPS 0x08u
#define HDA_PARAM_WIDGET_CAPS 0x09u
#define HDA_PARAM_PCM_RATES 0x0au
#define HDA_PARAM_STREAM_FORMATS 0x0bu
#define HDA_PARAM_PIN_CAPS 0x0cu
#define HDA_PARAM_AMP_INPUT_CAPS 0x0du
#define HDA_PARAM_CONNECTION_LIST_LENGTH 0x0eu
#define HDA_PARAM_AMP_OUTPUT_CAPS 0x12u

#define HDA_FUNCTION_GROUP_AUDIO 1u
#define HDA_CONNECTION_LONG_FORM 0x80u
#define HDA_CONNECTION_LENGTH_MASK 0x7fu
#define HDA_CONNECTION_RANGE_SHORT 0x80u
#define HDA_CONNECTION_MASK_SHORT 0x7fu
#define HDA_CONNECTION_RANGE_LONG 0x8000u
#define HDA_CONNECTION_MASK_LONG 0x7fffu

#define HDA_AMP_CAP_MUTE (1u << 31)
#define HDA_AMP_CAP_OFFSET_MASK 0x7fu
#define HDA_AMP_CAP_NUM_STEPS_SHIFT 8u
#define HDA_AMP_CAP_NUM_STEPS_MASK (0x7fu << HDA_AMP_CAP_NUM_STEPS_SHIFT)
#define HDA_AMP_SET_MUTE (1u << 7)
#define HDA_AMP_GAIN_MASK 0x7fu
#define HDA_AMP_GET_OUTPUT (1u << 15)
#define HDA_AMP_SET_OUTPUT (1u << 15)
#define HDA_AMP_SET_INPUT (1u << 14)
#define HDA_AMP_GET_LEFT (1u << 13)
#define HDA_AMP_SET_LEFT (1u << 13)
#define HDA_AMP_SET_RIGHT (1u << 12)
#define HDA_AMP_SET_INDEX_SHIFT 8u
#define HDA_AMP_GET_INDEX_MASK 0x0fu

#define HDA_POWER_ERROR (1u << 8)
#define HDA_POWER_ACT_SHIFT 4u
#define HDA_POWER_STATE_MASK 0x0fu
#define HDA_POWER_TRANSITION_TIMEOUT_TICKS 10u
#define HDA_POWER_TRANSITION_MAX_POLLS 16u

#define HDA_ROUTE_CHANGED_POWER (1u << 0)
#define HDA_ROUTE_CHANGED_SELECTOR (1u << 1)
#define HDA_ROUTE_CHANGED_PIN (1u << 2)
#define HDA_ROUTE_CHANGED_EAPD (1u << 3)
#define HDA_ROUTE_CHANGED_INPUT_AMP (1u << 4)
#define HDA_ROUTE_CHANGED_OUTPUT_AMP (1u << 5)

/** @brief Clear a bounded caller-owned CPU metadata region without libc imports.
 * @param storage Writable byte range; caller guarantees its size and lifetime.
 * @param byte_count Number of bytes to clear.
 * @return None; every byte in the requested range is set to zero.
 *
 * @par Context
 * Task-context metadata initialization and bounded route scratch setup.
 * @par Ownership and locks/IRQ
 * Caller retains the storage lease; this helper allocates nothing and takes no lock.
 */
static void hda_codec_zero(void *storage, uint64_t byte_count)
{
    uint8_t *bytes = storage;
    while (byte_count) {
        *bytes++ = 0u;
        --byte_count;
    }
}

/** @brief Query one codec parameter through the controller's serialized verb path.
 * @param c Initialized controller with a live codec transport.
 * @param cad Valid codec address, 0 through 14.
 * @param nid Node whose parameter is queried, including root NID zero.
 * @param parameter Parameter ID in the low eight bits.
 * @param out_value Receives the raw 32-bit response.
 * @return 0 or the original transport/validation errno.
 *
 * @par Context
 * Task-context discovery outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows controller state and output storage; hda_exec_verb owns its short flight and drops locks before waiting.
 */
static int hda_codec_get_parameter(struct hda_controller *c, uint8_t cad,
                                   uint8_t nid, uint8_t parameter,
                                   uint32_t *out_value)
{
    if (!c || !out_value || cad > HDA_CODEC_MAX_CAD) return -EINVAL;
    return hda_exec_verb(c, cad, nid, HDA_VERB_GET_PARAMETER, parameter,
                         false, out_value);
}

/** @brief Convert one checked subnode-count response into a bounded NID range.
 * @param c Controller used for the codec parameter query.
 * @param cad Valid codec address.
 * @param nid Root or function-group NID whose children are enumerated.
 * @param out_first Receives the first NID, from 0 through 255.
 * @param out_count Receives the number of children, from 0 through 255.
 * @return 0 or a transport error; -EINVAL for reserved bits or a range beyond NID 255.
 *
 * @par Context
 * Task-context discovery outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows controller and output pointers; no allocation or lock is held across the verb wait.
 */
static int hda_codec_read_node_range(struct hda_controller *c, uint8_t cad,
                                     uint8_t nid, uint8_t *out_first,
                                     uint8_t *out_count)
{
    uint32_t raw = 0u;
    int ret = hda_codec_get_parameter(c, cad, nid, HDA_PARAM_NODE_COUNT, &raw);
    if (ret) return ret;
    if ((raw & 0xff000000u) || (raw & 0x0000ff00u)) return -EINVAL;

    uint32_t first = (raw >> 16) & 0xffu;
    uint32_t count = raw & 0xffu;
    if (count && (!first || first + count > HDA_CODEC_MAX_NIDS)) return -EINVAL;
    if (out_first) *out_first = (uint8_t)first;
    if (out_count) *out_count = (uint8_t)count;
    return 0;
}

/** @brief Map a whole caller-owned metadata run only when every page is in the direct map.
 * @param phys Physical base returned by alloc_pages; must be nonzero and page aligned.
 * @param pages Exact page count of the already-owned contiguous CPU metadata run.
 * @return Base CPU alias, or NULL for invalid arithmetic, a missing page alias, or a noncontiguous direct-map result.
 *
 * @par Context
 * Task-context codec allocation before metadata is published.
 * @par Ownership and locks/IRQ
 * The codec remains owner of phys/pages and must call free_pages with the exact original pair. This helper creates no DMA mapping or allocation and holds no lock.
 */
static void *hda_codec_map_metadata_run(uint64_t phys, uint32_t pages)
{
    if (!phys || !pages || (phys & (HDA_CODEC_PAGE_BYTES - 1u))) return NULL;
    uint64_t bytes = (uint64_t)pages * HDA_CODEC_PAGE_BYTES;
    if (phys > UINT64_MAX - bytes) return NULL;

    void *base = hda_phys_to_direct_map_page(phys);
    if (!base) return NULL;
    uintptr_t base_address = (uintptr_t)base;
    if (bytes > UINTPTR_MAX - base_address) return NULL;
    for (uint32_t page = 1u; page < pages; ++page) {
        uint64_t offset = (uint64_t)page * HDA_CODEC_PAGE_BYTES;
        void *alias = hda_phys_to_direct_map_page(phys + offset);
        if (!alias || (uintptr_t)alias != base_address + (uintptr_t)offset)
            return NULL;
    }
    return base;
}

/** @brief Reclaim the exact CPU-only metadata runs held by a partial or full cache.
 * @param codec Cache whose physical bases and page counts are still intact.
 * @return None; each owned run is freed once with the original allocator tuple.
 *
 * @par Context
 * Task-context probe unwind or codec teardown after route users have stopped.
 * @par Ownership and locks/IRQ
 * Frees only codec-owned alloc_pages runs; never touches controller DMA/MMIO and never waits with a lock held.
 */
static void hda_codec_free_metadata(struct hda_codec *codec)
{
    if (!codec || !codec->api || !codec->api->free_pages) return;
    if (codec->connections_phys && codec->connections_pages) {
        codec->api->free_pages(codec->connections_phys, codec->connections_pages);
        codec->connections_phys = 0u;
        codec->connections_pages = 0u;
        codec->connections = NULL;
    }
    if (codec->nodes_phys && codec->nodes_pages) {
        codec->api->free_pages(codec->nodes_phys, codec->nodes_pages);
        codec->nodes_phys = 0u;
        codec->nodes_pages = 0u;
        codec->nodes = NULL;
    }
}

/** @brief Expand bounded HDA connection entries into their individual NIDs.
 * @param raw Raw short- or long-form connection list entries.
 * @param count Number of raw entries, at most 256.
 * @param long_form True for 15-bit NIDs with bit 15 as the range marker.
 * @param first First NID in the owning function group's declared widget range.
 * @param nodes Number of NIDs in that range; the exclusive end may be 256.
 * @param out Caller-owned output byte array.
 * @param capacity Number of bytes available in @p out.
 * @return Expanded entry count, -EINVAL for malformed/out-of-range data, or
 * -EOVERFLOW when the expanded list does not fit.
 *
 * @par Context
 * Task-context codec discovery; pure bounded conversion with no hardware access.
 * @par Ownership and locks/IRQ
 * Caller owns both arrays; no allocation, ownership transfer, lock, or IRQ action.
 */
int hda_expand_connections(const uint16_t *raw, uint32_t count, bool long_form,
                           uint8_t first, uint16_t nodes, uint8_t *out,
                           uint32_t capacity)
{
    if (count > HDA_CODEC_MAX_CONNECTIONS || (count && (!raw || !out)))
        return -EINVAL;
    uint32_t upper = (uint32_t)first + (uint32_t)nodes;
    if (upper > HDA_CODEC_MAX_NIDS || (count && !nodes)) return -EINVAL;
    if (!count) return 0;

    uint16_t range_bit = long_form ? HDA_CONNECTION_RANGE_LONG :
                                     HDA_CONNECTION_RANGE_SHORT;
    uint16_t nid_mask = long_form ? HDA_CONNECTION_MASK_LONG :
                                    HDA_CONNECTION_MASK_SHORT;
    uint32_t used = 0u;
    uint32_t previous = 0u;
    uint8_t have_previous = 0u;
    uint8_t previous_was_range = 0u;

    for (uint32_t i = 0u; i < count; ++i) {
        uint8_t is_range = (raw[i] & range_bit) != 0u;
        uint32_t end = raw[i] & nid_mask;
        uint32_t begin = end;
        if (is_range) {
            if (!have_previous || previous_was_range || previous >= 255u)
                return -EINVAL;
            begin = previous + 1u;
        }
        if (begin > end || begin < first || end >= upper || end > 255u)
            return -EINVAL;
        uint32_t amount = end - begin + 1u;
        if (amount > capacity - used) return -EOVERFLOW;
        for (uint32_t nid = begin; nid <= end; ++nid)
            out[used++] = (uint8_t)nid;
        previous = end;
        have_previous = 1u;
        previous_was_range = is_range;
    }
    return (int)used;
}

/** @brief Read and expand one widget's connection list into the codec-owned pool.
 * @param codec Partially initialized cache with its fixed connection pool allocated.
 * @param node Widget whose connection-list capability was read.
 * @param first First legal NID in this node's owning AFG widget range.
 * @param nodes Number of legal NIDs in that range.
 * @param used Current initialized byte count in the pool; updated on success.
 * @return 0 or transport, malformed-range, or pool-capacity error.
 *
 * @par Context
 * Task-context probe before the codec is published.
 * @par Ownership and locks/IRQ
 * Writes only within codec-owned CPU metadata; local raw data is bounded to 256 entries. No device DMA and no lock across verbs.
 */
static int hda_codec_load_connections(struct hda_codec *codec,
                                      struct hda_codec_node *node,
                                      uint8_t first, uint8_t nodes,
                                      uint32_t *used)
{
    node->connection_offset = *used;
    node->connection_count = 0u;
    if (!(node->widget_caps & HDA_WCAP_CONN_LIST)) return 0;

    uint32_t raw_length = 0u;
    int ret = hda_codec_get_parameter(codec->controller, codec->cad, node->nid,
                                      HDA_PARAM_CONNECTION_LIST_LENGTH,
                                      &raw_length);
    if (ret) return ret;
    if (raw_length & 0xffffff00u) return -EINVAL;
    uint32_t raw_count = raw_length & HDA_CONNECTION_LENGTH_MASK;
    bool long_form = (raw_length & HDA_CONNECTION_LONG_FORM) != 0u;
    if (raw_count > HDA_CODEC_MAX_CONNECTIONS) return -EINVAL;

    uint16_t raw[HDA_CODEC_MAX_CONNECTIONS];
    for (uint32_t i = 0u; i < raw_count;) {
        uint32_t response = 0u;
        ret = hda_exec_verb(codec->controller, codec->cad, node->nid,
                            HDA_VERB_GET_CONNECTION_LIST, (uint16_t)i, false,
                            &response);
        if (ret) return ret;
        uint32_t entries = long_form ? 2u : 4u;
        uint32_t width = long_form ? 16u : 8u;
        for (uint32_t slot = 0u; slot < entries && i < raw_count; ++slot, ++i)
            raw[i] = (uint16_t)(response >> (slot * width));
    }

    if (*used > HDA_CODEC_CONNECTION_BYTES) return -EOVERFLOW;
    uint32_t capacity = HDA_CODEC_CONNECTION_BYTES - *used;
    int expanded = hda_expand_connections(raw, raw_count, long_form, first,
        nodes, codec->connections + *used, capacity);
    if (expanded < 0) return expanded;
    if ((uint32_t)expanded > HDA_CODEC_MAX_CONNECTIONS) return -EOVERFLOW;
    node->connection_count = (uint16_t)expanded;
    *used += (uint32_t)expanded;
    return 0;
}

/** @brief Query a widget's pin, amplifier, format, and graph metadata.
 * @param codec Partially initialized codec cache owning node and connection storage.
 * @param node Destination node record for the current NID.
 * @param widget_range_start First NID in this AFG's widget range.
 * @param widget_range_count Number of NIDs in this AFG's widget range.
 * @param connection_bytes Current expanded-pool allocation cursor.
 * @return 0 or the first transport, malformed topology, or capacity error.
 *
 * @par Context
 * Task-context probe outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Mutates only unpublished codec metadata; inherits raw AFG capability bits where override flags are clear. No lock is held across verbs.
 */
static int hda_codec_probe_widget(struct hda_codec *codec,
                                  struct hda_codec_node *node,
                                  uint8_t widget_range_start,
                                  uint8_t widget_range_count,
                                  uint32_t *connection_bytes)
{
    uint32_t value = 0u;
    int ret = hda_codec_get_parameter(codec->controller, codec->cad, node->nid,
                                      HDA_PARAM_WIDGET_CAPS, &value);
    if (ret) return ret;
    node->widget_caps = value;
    node->type = (uint8_t)((value >> 20) & 0x0fu);

    if (node->type == HDA_WIDGET_PIN) {
        ret = hda_codec_get_parameter(codec->controller, codec->cad, node->nid,
                                      HDA_PARAM_PIN_CAPS, &node->pin_caps);
        if (ret) return ret;
        ret = hda_exec_verb(codec->controller, codec->cad, node->nid,
                            HDA_VERB_GET_CONFIG_DEFAULT, 0u, false,
                            &node->pin_default);
        if (ret) return ret;
    }

    ret = hda_codec_load_connections(codec, node, widget_range_start,
                                     widget_range_count, connection_bytes);
    if (ret) return ret;

    struct hda_codec_node *afg = &codec->nodes[node->afg_nid];
    bool amp_override = (node->widget_caps & HDA_WCAP_AMP_OVERRIDE) != 0u;
    if (node->widget_caps & HDA_WCAP_IN_AMP) {
        if (amp_override) {
            ret = hda_codec_get_parameter(codec->controller, codec->cad,
                                          node->nid, HDA_PARAM_AMP_INPUT_CAPS,
                                          &node->amp_input_caps);
            if (ret) return ret;
        } else {
            node->amp_input_caps = afg->amp_input_caps;
        }
        uint32_t input_steps = (node->amp_input_caps &
            HDA_AMP_CAP_NUM_STEPS_MASK) >> HDA_AMP_CAP_NUM_STEPS_SHIFT;
        if ((node->amp_input_caps & HDA_AMP_CAP_OFFSET_MASK) > input_steps)
            return -ERANGE;
    }
    if (node->widget_caps & HDA_WCAP_OUT_AMP) {
        if (amp_override) {
            ret = hda_codec_get_parameter(codec->controller, codec->cad,
                                          node->nid, HDA_PARAM_AMP_OUTPUT_CAPS,
                                          &node->amp_output_caps);
            if (ret) return ret;
        } else {
            node->amp_output_caps = afg->amp_output_caps;
        }
        uint32_t output_steps = (node->amp_output_caps &
            HDA_AMP_CAP_NUM_STEPS_MASK) >> HDA_AMP_CAP_NUM_STEPS_SHIFT;
        if ((node->amp_output_caps & HDA_AMP_CAP_OFFSET_MASK) > output_steps)
            return -ERANGE;
    }

    if (node->type == HDA_WIDGET_AUDIO_OUTPUT ||
        node->type == HDA_WIDGET_AUDIO_INPUT) {
        if (node->widget_caps & HDA_WCAP_FORMAT_OVERRIDE) {
            ret = hda_codec_get_parameter(codec->controller, codec->cad,
                                          node->nid, HDA_PARAM_PCM_RATES,
                                          &node->pcm_rates);
            if (ret) return ret;
            ret = hda_codec_get_parameter(codec->controller, codec->cad,
                                          node->nid, HDA_PARAM_STREAM_FORMATS,
                                          &node->stream_formats);
            if (ret) return ret;
        } else {
            node->pcm_rates = afg->pcm_rates;
            node->stream_formats = afg->stream_formats;
        }
    }
    return 0;
}

/** @brief Discover generic audio function groups and cache each real analog route capability.
 * @param c Initialized controller whose codec mask contains @p cad.
 * @param cad Codec address in the legal range 0..14; 15 is reserved by the link.
 * @param codec Caller-owned zero-initialized metadata output.
 * @return 0, -EINVAL for invalid CAD/topology, -ENOMEM for metadata failure, or
 * the first propagated command error. Every failed probe releases its CPU pages.
 *
 * @par Context
 * Task-context initialization outside global execution ownership; not an IRQ/service operation.
 * @par Ownership and locks/IRQ
 * Success transfers exact alloc_pages runs to codec until destroy; they are CPU metadata only, never device DMA. No controller state lock is held across verbs.
 */
int hda_codec_probe(struct hda_controller *c, uint8_t cad,
                    struct hda_codec *codec)
{
    if (!c || !codec || cad > HDA_CODEC_MAX_CAD || !c->initialized ||
        c->transport_mode == HDA_TRANSPORT_UNAVAILABLE ||
        !(c->codec_mask & (1u << cad)) || !c->api || !c->api->alloc_pages ||
        !c->api->free_pages) return -EINVAL;
    if (codec->nodes || codec->connections || codec->nodes_phys ||
        codec->connections_phys || codec->probed) return -EBUSY;

    hda_codec_zero(codec, sizeof(*codec));
    codec->controller = c;
    codec->api = c->api;
    codec->cad = cad;

    int ret = hda_codec_get_parameter(c, cad, HDA_ROOT_NID,
                                      HDA_PARAM_VENDOR_ID, &codec->vendor_id);
    if (ret) goto fail;
    if (!codec->vendor_id || codec->vendor_id == UINT32_MAX) {
        ret = -ENODEV;
        goto fail;
    }
    ret = hda_codec_get_parameter(c, cad, HDA_ROOT_NID,
                                  HDA_PARAM_REVISION_ID, &codec->revision_id);
    if (ret) goto fail;
    ret = hda_codec_read_node_range(c, cad, HDA_ROOT_NID,
                                    &codec->root_fg_start,
                                    &codec->root_fg_count);
    if (ret) goto fail;

    uint64_t node_bytes = (uint64_t)HDA_CODEC_MAX_NIDS *
                          (uint64_t)sizeof(struct hda_codec_node);
    uint64_t node_pages64 = (node_bytes + HDA_CODEC_PAGE_BYTES - 1u) /
                            HDA_CODEC_PAGE_BYTES;
    uint64_t connection_pages64 =
        (HDA_CODEC_CONNECTION_BYTES + HDA_CODEC_PAGE_BYTES - 1u) /
        HDA_CODEC_PAGE_BYTES;
    if (!node_pages64 || node_pages64 > UINT32_MAX || !connection_pages64 ||
        connection_pages64 > UINT32_MAX) {
        ret = -EOVERFLOW;
        goto fail;
    }
    codec->nodes_pages = (uint32_t)node_pages64;
    codec->nodes_phys = c->api->alloc_pages(codec->nodes_pages);
    if (!codec->nodes_phys) {
        codec->nodes_pages = 0u;
        ret = -ENOMEM;
        goto fail;
    }
    codec->nodes = hda_codec_map_metadata_run(codec->nodes_phys,
                                              codec->nodes_pages);
    if (!codec->nodes) {
        ret = -ERANGE;
        goto fail;
    }
    hda_codec_zero(codec->nodes, node_pages64 * HDA_CODEC_PAGE_BYTES);

    codec->connections_pages = (uint32_t)connection_pages64;
    codec->connections_phys = c->api->alloc_pages(codec->connections_pages);
    if (!codec->connections_phys) {
        codec->connections_pages = 0u;
        ret = -ENOMEM;
        goto fail;
    }
    codec->connections = hda_codec_map_metadata_run(codec->connections_phys,
                                                    codec->connections_pages);
    if (!codec->connections) {
        ret = -ERANGE;
        goto fail;
    }
    hda_codec_zero(codec->connections,
                   connection_pages64 * HDA_CODEC_PAGE_BYTES);

    uint32_t connection_bytes = 0u;
    uint8_t first_audio_fg = 0u;
    for (uint32_t fg_offset = 0u; fg_offset < codec->root_fg_count; ++fg_offset) {
        uint32_t fg_nid_wide = (uint32_t)codec->root_fg_start + fg_offset;
        if (fg_nid_wide >= HDA_CODEC_MAX_NIDS) {
            ret = -EINVAL;
            goto fail;
        }
        uint8_t fg_nid = (uint8_t)fg_nid_wide;
        uint32_t function_type = 0u;
        ret = hda_codec_get_parameter(c, cad, fg_nid,
                                      HDA_PARAM_FUNCTION_TYPE,
                                      &function_type);
        if (ret) goto fail;
        if ((function_type & 0xffu) != HDA_FUNCTION_GROUP_AUDIO) continue;
        if (codec->nodes[fg_nid].present) {
            ret = -EINVAL;
            goto fail;
        }

        struct hda_codec_node *afg = &codec->nodes[fg_nid];
        afg->nid = fg_nid;
        afg->type = HDA_WIDGET_FUNCTION_GROUP;
        afg->afg_nid = fg_nid;
        afg->present = 1u;
        ret = hda_exec_verb(c, cad, fg_nid,
                            HDA_VERB_GET_IMPLEMENTATION_ID, 0u, false,
                            &afg->implementation_id);
        if (ret) goto fail;
        ret = hda_codec_get_parameter(c, cad, fg_nid, HDA_PARAM_AFG_CAPS,
                                      &codec->afg_caps);
        if (ret) goto fail;
        afg->widget_caps = codec->afg_caps;
        ret = hda_codec_get_parameter(c, cad, fg_nid, HDA_PARAM_PCM_RATES,
                                      &afg->pcm_rates);
        if (ret) goto fail;
        ret = hda_codec_get_parameter(c, cad, fg_nid,
                                      HDA_PARAM_STREAM_FORMATS,
                                      &afg->stream_formats);
        if (ret) goto fail;
        ret = hda_codec_get_parameter(c, cad, fg_nid,
                                      HDA_PARAM_AMP_INPUT_CAPS,
                                      &afg->amp_input_caps);
        if (ret) goto fail;
        ret = hda_codec_get_parameter(c, cad, fg_nid,
                                      HDA_PARAM_AMP_OUTPUT_CAPS,
                                      &afg->amp_output_caps);
        if (ret) goto fail;
        if (!first_audio_fg) {
            first_audio_fg = fg_nid;
            codec->subsystem_id = afg->implementation_id;
            codec->afg_pcm_rates = afg->pcm_rates;
            codec->afg_stream_formats = afg->stream_formats;
            codec->afg_amp_input_caps = afg->amp_input_caps;
            codec->afg_amp_output_caps = afg->amp_output_caps;
        }

        uint8_t widget_first = 0u;
        uint8_t widget_count = 0u;
        ret = hda_codec_read_node_range(c, cad, fg_nid,
                                        &widget_first, &widget_count);
        if (ret) goto fail;
        uint32_t widget_end = (uint32_t)widget_first + widget_count;
        if (widget_count && (!widget_first || widget_end > HDA_CODEC_MAX_NIDS)) {
            ret = -EINVAL;
            goto fail;
        }
        for (uint32_t widget_offset = 0u; widget_offset < widget_count;
             ++widget_offset) {
            uint32_t nid_wide = (uint32_t)widget_first + widget_offset;
            if (nid_wide >= HDA_CODEC_MAX_NIDS) {
                ret = -EINVAL;
                goto fail;
            }
            uint8_t nid = (uint8_t)nid_wide;
            struct hda_codec_node *node = &codec->nodes[nid];
            if (node->present) {
                ret = -EINVAL;
                goto fail;
            }
            node->nid = nid;
            node->afg_nid = fg_nid;
            node->present = 1u;
            ret = hda_codec_probe_widget(codec, node, widget_first,
                                          widget_count, &connection_bytes);
            if (ret) goto fail;
        }
    }

    codec->probed = 1u;
    struct hda_route route;
    for (uint32_t nid = 1u; nid < HDA_CODEC_MAX_NIDS; ++nid) {
        struct hda_codec_node *node = &codec->nodes[nid];
        if (!node->present || node->type != HDA_WIDGET_PIN) continue;
        if (node->pin_caps & HDA_PINCAP_OUTPUT) {
            hda_codec_zero(&route, sizeof(route));
            if (!hda_find_route(codec, (uint8_t)nid, HDA_ROUTE_PLAYBACK,
                                &route)) {
                struct hda_codec_node *converter =
                    &codec->nodes[route.converter_nid];
                if ((converter->stream_formats & 1u) &&
                    (converter->pcm_rates & 0x0fffu)) {
                    codec->has_playback = 1u;
                    codec->playback_pcm_rates |= converter->pcm_rates;
                    codec->playback_stream_formats |= converter->stream_formats;
                }
            }
        }
        if (node->pin_caps & HDA_PINCAP_INPUT) {
            hda_codec_zero(&route, sizeof(route));
            if (!hda_find_route(codec, (uint8_t)nid, HDA_ROUTE_CAPTURE,
                                &route)) {
                struct hda_codec_node *converter =
                    &codec->nodes[route.converter_nid];
                if ((converter->stream_formats & 1u) &&
                    (converter->pcm_rates & 0x0fffu)) {
                    codec->has_capture = 1u;
                    codec->capture_pcm_rates |= converter->pcm_rates;
                    codec->capture_stream_formats |= converter->stream_formats;
                }
            }
        }
    }
    return 0;

fail:
    hda_codec_free_metadata(codec);
    hda_codec_zero(codec, sizeof(*codec));
    return ret;
}

/** @brief Enter a per-codec route transaction without holding a lock over verbs.
 * @param codec Cache whose atomic logical gate is acquired.
 * @return 0 when acquired or -EBUSY if another full route transaction is active.
 *
 * @par Context
 * Task context before a synchronous route operation.
 * @par Ownership and locks/IRQ
 * Uses one bounded atomic compare/exchange; it never spins, waits, disables IRQs, or holds a spinlock across a verb.
 */
static int hda_codec_route_gate_enter(struct hda_codec *codec)
{
    uint32_t expected = 0u;
    return __atomic_compare_exchange_n(&codec->route_busy, &expected, 1u, false,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ?
           0 : -EBUSY;
}

/** @brief Leave the per-codec logical route transaction.
 * @param codec Cache whose route transaction gate is currently owned by caller.
 * @return None; publishes completion to a future transaction.
 *
 * @par Context
 * Task context at every route transaction exit.
 * @par Ownership and locks/IRQ
 * Releases one atomic logical gate; no hardware access or lock wait occurs.
 */
static void hda_codec_route_gate_leave(struct hda_codec *codec)
{
    __atomic_store_n(&codec->route_busy, 0u, __ATOMIC_RELEASE);
}

/** @brief Enter the codec transaction gate for a task-context mixer operation.
 * @param codec Codec cache whose route/control state is being serialized.
 * @return 0 when the gate is acquired, -EINVAL for a missing cache, or -EBUSY
 * when another route/control transaction owns it.
 * @par Context Task context only; no IRQ or audio-service callback may wait.
 * @par Ownership and locks/IRQ Caller owns the gate until the matching leave;
 * this is an atomic logical gate and no hardware lock spans codec verbs.
 */
int hda_codec_control_gate_enter(struct hda_codec *codec)
{
    return codec ? hda_codec_route_gate_enter(codec) : -EINVAL;
}

/** @brief Release a mixer/route transaction gate previously acquired by caller.
 * @param codec Codec cache whose gate is currently owned by the caller.
 * @return None; a null cache is ignored defensively.
 * @par Context Task context after the synchronous control operation completes.
 * @par Ownership and locks/IRQ Caller must own the logical gate; no hardware
 * access or wait occurs and IRQ state is unchanged.
 */
void hda_codec_control_gate_leave(struct hda_codec *codec)
{
    if (codec) hda_codec_route_gate_leave(codec);
}

/** @brief Release all metadata after confirming no route owns the cache.
 * @param codec Probed or partially initialized cache; no route may be active.
 * @return 0 after exact CPU-page reclamation, -EBUSY during a route transaction or with active routes.
 *
 * @par Context
 * Task teardown after streams have stopped and routes have been restored.
 * @par Ownership and locks/IRQ
 * Uses a bounded atomic gate, frees only codec-owned CPU metadata pages, and leaves controller transport ownership unchanged.
 */
int hda_codec_destroy(struct hda_codec *codec)
{
    if (!codec) return -EINVAL;
    int ret = hda_codec_route_gate_enter(codec);
    if (ret) return ret;
    if (codec->active_routes || codec->route_groups) {
        hda_codec_route_gate_leave(codec);
        return -EBUSY;
    }
    hda_codec_free_metadata(codec);
    hda_codec_zero(codec, sizeof(*codec));
    return 0;
}

/** @brief Validate and return a node's expanded connection slice.
 * @param codec Cache whose pool owns the node's expanded connections.
 * @param node Parsed node with an offset/count pair to validate.
 * @param out Receives the borrowed connection slice (NULL when count is zero).
 * @return 0 or -EINVAL when the slice would escape the fixed codec pool.
 *
 * @par Context
 * Task-context route search over immutable probed metadata.
 * @par Ownership and locks/IRQ
 * Returned bytes are borrowed from codec and remain valid only while it is alive; no lock or allocation.
 */
static int hda_codec_connection_slice(const struct hda_codec *codec,
                                     const struct hda_codec_node *node,
                                     const uint8_t **out)
{
    if (!codec || !node || !out || node->connection_count > HDA_CODEC_MAX_CONNECTIONS ||
        node->connection_offset > HDA_CODEC_CONNECTION_BYTES ||
        node->connection_count > HDA_CODEC_CONNECTION_BYTES - node->connection_offset)
        return -EINVAL;
    *out = node->connection_count ? codec->connections + node->connection_offset : NULL;
    return 0;
}

/** @brief Admit only analog signal nodes for one route direction.
 * @param node Parsed widget candidate from the same AFG.
 * @param direction Playback or capture direction being traversed.
 * @return True for analog pin/mixer/selector widgets and the matching analog converter.
 *
 * @par Context
 * Task-context graph traversal over immutable codec metadata.
 * @par Ownership and locks/IRQ
 * Reads borrowed metadata only; no allocation, mutation, lock, or device access.
 */
static bool hda_codec_is_analog_path_node(const struct hda_codec_node *node,
                                         enum hda_route_direction direction)
{
    if (!node || !node->present || (node->widget_caps & HDA_WCAP_DIGITAL))
        return false;
    if (node->type == HDA_WIDGET_PIN || node->type == HDA_WIDGET_MIXER ||
        node->type == HDA_WIDGET_SELECTOR) return true;
    if (direction == HDA_ROUTE_PLAYBACK)
        return node->type == HDA_WIDGET_AUDIO_OUTPUT;
    return node->type == HDA_WIDGET_AUDIO_INPUT;
}

/** @brief Resolve the actual same-AFG directed connection path for one pin.
 * @param codec Successfully probed codec with immutable node/connection metadata.
 * @param pin Requested Pin Complex NID.
 * @param direction Playback traverses pin connection lists toward DACs; capture
 * traverses consumer lists backward from the pin until an ADC is found.
 * @param route Caller-owned output path and later control snapshot storage.
 * @param blocked Nodes unavailable to this group member, or NULL for an
 * unconstrained single-pin search.
 * @param used_converters Converters already assigned to earlier members, or
 * NULL for a single-pin search.
 * @param skip_converter Optional converter NID to exclude while enumerating
 * bounded alternatives, or zero for the first candidate.
 * @return 0 with a converter path, -ENODEV if no live graph path exists, or
 * -EINVAL for invalid codec, NID, direction, pin type, or pin direction capability.
 *
 * @par Context
 * Task context; the graph walk is iterative, bounded to 256 NIDs, and performs no verbs or waits.
 * @par Ownership and locks/IRQ
 * route borrows codec; caller keeps the cache alive and serializes route-object use. Visited and BFS arrays are bounded local scratch only.
 */
static int hda_find_route_graph(struct hda_codec *codec, uint8_t pin,
                   enum hda_route_direction direction,
                   struct hda_route *route, const uint8_t *blocked,
                   const uint8_t *used_converters, uint8_t skip_converter)
{
    if (!codec || !codec->probed || !codec->nodes || !codec->connections ||
        !route || !pin || direction > HDA_ROUTE_CAPTURE) return -EINVAL;
    if (route->active || route->snapshots_valid) return -EBUSY;
    struct hda_codec_node *pin_node = &codec->nodes[pin];
    if (!pin_node->present || pin_node->type != HDA_WIDGET_PIN) return -EINVAL;
    if (pin_node->widget_caps & HDA_WCAP_DIGITAL) return -ENODEV;
    if ((direction == HDA_ROUTE_PLAYBACK &&
         !(pin_node->pin_caps & HDA_PINCAP_OUTPUT)) ||
        (direction == HDA_ROUTE_CAPTURE &&
         !(pin_node->pin_caps & HDA_PINCAP_INPUT))) return -EINVAL;

    uint8_t queue[HDA_CODEC_MAX_NIDS];
    uint8_t parent[HDA_CODEC_MAX_NIDS];
    uint8_t parent_connection[HDA_CODEC_MAX_NIDS];
    uint8_t visited[HDA_CODEC_MAX_NIDS];
    uint8_t has_parent[HDA_CODEC_MAX_NIDS];
    hda_codec_zero(visited, sizeof(visited));
    hda_codec_zero(has_parent, sizeof(has_parent));
    uint32_t head = 0u;
    uint32_t tail = 0u;
    queue[tail++] = pin;
    visited[pin] = 1u;
    uint8_t target = 0u;

    while (head < tail) {
        uint8_t current = queue[head++];
        struct hda_codec_node *current_node = &codec->nodes[current];
        if (direction == HDA_ROUTE_PLAYBACK) {
            if (current != pin && current_node->type == HDA_WIDGET_AUDIO_OUTPUT) {
                if (current != skip_converter &&
                    (!used_converters || !used_converters[current])) {
                    target = current;
                    break;
                }
                continue;
            }
            const uint8_t *connections = NULL;
            int ret = hda_codec_connection_slice(codec, current_node, &connections);
            if (ret) return ret;
            for (uint32_t i = 0u; i < current_node->connection_count; ++i) {
                uint8_t next = connections[i];
                struct hda_codec_node *next_node = &codec->nodes[next];
                if (!hda_codec_is_analog_path_node(next_node, direction) ||
                    next_node->afg_nid != pin_node->afg_nid ||
                    (blocked && blocked[next]) ||
                    (next_node->type == HDA_WIDGET_AUDIO_OUTPUT &&
                     ((used_converters && used_converters[next]) ||
                      next == skip_converter)) ||
                    visited[next]) continue;
                visited[next] = 1u;
                has_parent[next] = 1u;
                parent[next] = current;
                parent_connection[next] = (uint8_t)i;
                queue[tail++] = next;
            }
        } else {
            if (current != pin && current_node->type == HDA_WIDGET_AUDIO_INPUT) {
                if (current != skip_converter &&
                    (!used_converters || !used_converters[current])) {
                    target = current;
                    break;
                }
                continue;
            }
            for (uint32_t candidate = 1u; candidate < HDA_CODEC_MAX_NIDS;
                 ++candidate) {
                struct hda_codec_node *consumer = &codec->nodes[candidate];
                if (!hda_codec_is_analog_path_node(consumer, direction) ||
                    consumer->afg_nid != pin_node->afg_nid ||
                    (blocked && blocked[candidate]) ||
                    (consumer->type == HDA_WIDGET_AUDIO_INPUT &&
                     ((used_converters && used_converters[candidate]) ||
                      candidate == skip_converter)) ||
                    visited[candidate]) continue;
                const uint8_t *connections = NULL;
                int ret = hda_codec_connection_slice(codec, consumer, &connections);
                if (ret) return ret;
                for (uint32_t i = 0u; i < consumer->connection_count; ++i) {
                    if (connections[i] != current) continue;
                    visited[candidate] = 1u;
                    has_parent[candidate] = 1u;
                    parent[candidate] = current;
                    parent_connection[candidate] = (uint8_t)i;
                    queue[tail++] = (uint8_t)candidate;
                    break;
                }
            }
        }
        if (tail > HDA_CODEC_MAX_NIDS) return -EOVERFLOW;
    }
    if (!target) return -ENODEV;

    uint8_t reverse_path[HDA_ROUTE_MAX_NODES];
    uint32_t path_count = 0u;
    uint8_t cursor = target;
    reverse_path[path_count++] = cursor;
    while (cursor != pin) {
        if (!has_parent[cursor] || path_count >= HDA_ROUTE_MAX_NODES)
            return -EINVAL;
        cursor = parent[cursor];
        reverse_path[path_count++] = cursor;
    }
    if (path_count < 2u || path_count > HDA_ROUTE_MAX_NODES) return -EINVAL;

    hda_codec_zero(route, sizeof(*route));
    route->codec = codec;
    route->direction = (uint8_t)direction;
    route->pin_nid = pin;
    route->converter_nid = target;
    route->path_count = (uint16_t)path_count;
    route->association = (uint8_t)((pin_node->pin_default >> 4) & 0x0fu);
    route->sequence = (uint8_t)(pin_node->pin_default & 0x0fu);
    if (direction == HDA_ROUTE_PLAYBACK) {
        for (uint32_t i = 0u; i < path_count; ++i)
            route->path[i] = reverse_path[path_count - i - 1u];
        for (uint32_t i = 0u; i + 1u < path_count; ++i)
            route->connection_index[i] =
                parent_connection[route->path[i + 1u]];
    } else {
        for (uint32_t i = 0u; i < path_count; ++i)
            route->path[i] = reverse_path[i];
        for (uint32_t i = 0u; i + 1u < path_count; ++i)
            route->connection_index[i] =
                parent_connection[route->path[i]];
    }
    route->connection_index[path_count - 1u] = 0xffu;
    return 0;
}

/** @brief Resolve one unconstrained analog route for a pin.
 * @param codec Probed codec with immutable parsed graph metadata.
 * @param pin Requested Pin Complex NID.
 * @param direction Playback searches pin-to-DAC; capture searches ADC-to-pin.
 * @param route Caller-owned output path and later snapshot storage.
 * @return 0 with a real same-AFG analog path or the graph validation error.
 *
 * @par Context
 * Task context; bounded graph traversal performs no verbs or waits.
 * @par Ownership and locks/IRQ
 * route borrows codec, and caller serializes route-object reuse and codec
 * teardown. The function mutates only caller-owned route bytes.
 */
int hda_find_route(struct hda_codec *codec, uint8_t pin,
                   enum hda_route_direction direction,
                   struct hda_route *route)
{
    return hda_find_route_graph(codec, pin, direction, route, NULL, NULL, 0u);
}

/** @brief Validate a caller-owned route against the immutable parsed graph.
 * @param codec Probed codec whose route gate is held by the caller.
 * @param route Route path and edge indices to validate before any hardware access.
 * @return 0 for a same-AFG analog path whose edges still match, otherwise -EINVAL.
 *
 * @par Context
 * Task context while the full route transaction gate is owned.
 * @par Ownership and locks/IRQ
 * Reads only codec-owned metadata and caller-owned path bytes; no allocation, mutation, lock, or verb wait.
 */
static int hda_route_validate(const struct hda_codec *codec,
                              const struct hda_route *route)
{
    if (!codec || !route || !codec->probed || !codec->nodes ||
        !codec->connections || route->codec != codec ||
        route->path_count < 2u || route->path_count > HDA_ROUTE_MAX_NODES ||
        route->direction > HDA_ROUTE_CAPTURE || !route->pin_nid ||
        !route->converter_nid) return -EINVAL;
    const struct hda_codec_node *pin = &codec->nodes[route->pin_nid];
    if (!pin->present || pin->type != HDA_WIDGET_PIN ||
        (pin->widget_caps & HDA_WCAP_DIGITAL) ||
        route->association != ((pin->pin_default >> 4) & 0x0fu) ||
        route->sequence != (pin->pin_default & 0x0fu)) return -EINVAL;
    const struct hda_codec_node *converter =
        &codec->nodes[route->converter_nid];
    uint8_t wanted_converter = route->direction == HDA_ROUTE_PLAYBACK ?
        HDA_WIDGET_AUDIO_OUTPUT : HDA_WIDGET_AUDIO_INPUT;
    if (!converter->present || converter->type != wanted_converter ||
        (converter->widget_caps & HDA_WCAP_DIGITAL)) return -EINVAL;
    if ((route->direction == HDA_ROUTE_PLAYBACK &&
         (route->path[0] != route->pin_nid ||
          route->path[route->path_count - 1u] != route->converter_nid)) ||
        (route->direction == HDA_ROUTE_CAPTURE &&
         (route->path[0] != route->converter_nid ||
          route->path[route->path_count - 1u] != route->pin_nid))) return -EINVAL;
    const struct hda_codec_node *afg = &codec->nodes[pin->afg_nid];
    if (!afg->present || afg->type != HDA_WIDGET_FUNCTION_GROUP) return -EINVAL;

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        uint8_t nid = route->path[i];
        const struct hda_codec_node *node = &codec->nodes[nid];
        if (!hda_codec_is_analog_path_node(node,
                (enum hda_route_direction)route->direction) ||
            node->afg_nid != pin->afg_nid) return -EINVAL;
        for (uint32_t previous = 0u; previous < i; ++previous)
            if (route->path[previous] == nid) return -EINVAL;
        if (i + 1u == route->path_count) {
            if (route->connection_index[i] != 0xffu) return -EINVAL;
            continue;
        }
        const uint8_t *connections = NULL;
        if (hda_codec_connection_slice(codec, node, &connections) ||
            route->connection_index[i] >= node->connection_count ||
            connections[route->connection_index[i]] != route->path[i + 1u])
            return -EINVAL;
    }
    return 0;
}

/** @brief Reject paths that would share an already leased signal widget.
 * @param codec Probed cache whose logical route gate is held.
 * @param route Validated path to check before any route write.
 * @return 0 if every node is free, -EBUSY for an active conflicting route.
 *
 * @par Context
 * Task context within a serialized codec route transaction.
 * @par Ownership and locks/IRQ
 * Reads codec-owned ownership bytes only; it performs no allocation or I/O.
 */
static int hda_route_check_control_lease(const struct hda_codec *codec,
                                         const struct hda_route *route)
{
    if (route->control_lease_acquired) return -EBUSY;
    for (uint32_t i = 0u; i < route->path_count; ++i)
        if (codec->route_node_owner[route->path[i]]) return -EBUSY;
    return 0;
}

/** @brief Decode the 1-based channel count encoded by WidgetCaps.
 * @param caps Raw widget capabilities.
 * @return Number of supported channels in the range 1..16.
 *
 * @par Context
 * Task-context topology planning over immutable cached capabilities.
 * @par Ownership and locks/IRQ
 * Pure bit extraction; no mutation, allocation, lock, or I/O.
 */
static uint8_t hda_codec_channel_count(uint32_t caps)
{
    uint32_t encoded = (((caps >> 13) & 7u) << 1) | (caps & 1u);
    return (uint8_t)(encoded + 1u);
}

/** @brief Compute the narrowest channel capacity along a resolved path.
 * @param codec Probed codec whose immutable widget capabilities are borrowed.
 * @param route Validated route whose path includes pin and converter.
 * @return Minimum channel count across every path widget, or zero for invalid input.
 *
 * @par Context
 * Task-context group planning; no verbs, allocation, or waits.
 * @par Ownership and locks/IRQ
 * Reads codec metadata and caller-owned route bytes only.
 */
static uint8_t hda_route_path_capacity(const struct hda_codec *codec,
                                       const struct hda_route *route)
{
    if (!codec || !route || !route->path_count ||
        route->path_count > HDA_ROUTE_MAX_NODES) return 0u;
    uint8_t capacity = 16u;
    for (uint32_t i = 0u; i < route->path_count; ++i) {
        uint8_t node_capacity = hda_codec_channel_count(
            codec->nodes[route->path[i]].widget_caps);
        if (node_capacity < capacity) capacity = node_capacity;
    }
    return capacity;
}

/** @brief Reject paths whose selected input amplifier index is not wire encodable.
 * @param codec Probed codec with immutable widget capabilities.
 * @param route Validated route whose path and connection indices are inspected.
 * @return 0 when every input amplifier index fits the four-bit HDA field, or
 * -ERANGE when group discovery would overstate a path as applicable.
 *
 * @par Context
 * Task-context group planning and validation; no verbs, allocation, or waits.
 * @par Ownership and locks/IRQ
 * Reads codec metadata and caller-owned route bytes only.
 */
static int hda_route_group_amp_indices(const struct hda_codec *codec,
                                       const struct hda_route *route)
{
    if (!codec || !route) return -EINVAL;
    for (uint32_t i = 0u; i + 1u < route->path_count; ++i) {
        const struct hda_codec_node *node = &codec->nodes[route->path[i]];
        if (node->amp_input_caps && route->connection_index[i] > 15u)
            return -ERANGE;
    }
    return 0;
}

/** @brief Bounded depth-first assignment of sorted association members.
 * @param codec Probed codec with immutable graph metadata.
 * @param sorted_pins Sequence-sorted output pins to assign.
 * @param member_count Number of members in sorted_pins and members.
 * @param index Current member depth.
 * @param total_channels Channels assigned before index.
 * @param members Page-backed route slots owned by the caller.
 * @param blocked Signal widgets assigned by earlier members.
 * @param used_converters Converter ownership bitmap.
 * @param budget Remaining bounded candidate attempts.
 * @return 0 for a complete assignment, -ENODEV when no bounded assignment
 * exists, or a graph/metadata error that must abort planning.
 *
 * @par Context
 * Task-context planning with at most 16 depths and 256 converter exclusions
 * per depth; no verbs or waits.
 * @par Ownership and locks/IRQ
 * Mutates only page-backed route slots and caller-owned scratch bitmaps.
 */
static int hda_route_group_solve(const struct hda_codec *codec,
                                 const uint8_t *sorted_pins,
                                 uint32_t member_count, uint32_t index,
                                 uint32_t total_channels,
                                 struct hda_route *members,
                                 uint8_t *blocked,
                                 uint8_t *used_converters,
                                 uint32_t *budget)
{
    if (index == member_count) return 0;
    if (total_channels >= 16u) return -EOVERFLOW;
    if (!budget || !*budget) return -ENODEV;
    struct hda_codec *mutable_codec = (struct hda_codec *)codec;
    struct hda_route *route = &members[index];
    int last_error = -ENODEV;
    bool saw_overflow = false;
    uint8_t afg_nid = codec->nodes[sorted_pins[index]].afg_nid;
    for (uint32_t skip_index = 0u; skip_index <= HDA_CODEC_MAX_NIDS;
         ++skip_index) {
        uint8_t skip = 0u;
        if (skip_index) {
            uint8_t candidate = (uint8_t)(skip_index - 1u);
            const struct hda_codec_node *candidate_node =
                &codec->nodes[candidate];
            if (!candidate_node->present ||
                candidate_node->afg_nid != afg_nid ||
                candidate_node->type != HDA_WIDGET_AUDIO_OUTPUT)
                continue;
            skip = candidate;
        }
        if (!*budget) return saw_overflow ? -EOVERFLOW : -ENODEV;
        --*budget;
        hda_codec_zero(route, sizeof(*route));
        int ret = hda_find_route_graph(mutable_codec, sorted_pins[index],
                                       HDA_ROUTE_PLAYBACK, route, blocked,
                                       used_converters, (uint8_t)skip);
        if (ret == -ENODEV) continue;
        if (ret) return ret;
        ret = hda_route_validate(codec, route);
        if (ret) return ret;
        ret = hda_route_group_amp_indices(codec, route);
        if (ret) {
            last_error = ret;
            continue;
        }
        uint8_t channels = hda_route_path_capacity(codec, route);
        if (!channels || total_channels + channels > 16u) {
            last_error = -EOVERFLOW;
            saw_overflow = true;
            continue;
        }
        route->channel_start = (uint8_t)total_channels;
        route->channel_count = channels;
        for (uint32_t path = 0u; path < route->path_count; ++path)
            blocked[route->path[path]] = 1u;
        used_converters[route->converter_nid] = 1u;
        ret = hda_route_group_solve(codec, sorted_pins, member_count,
                                    index + 1u, total_channels + channels,
                                    members, blocked, used_converters, budget);
        if (!ret) return 0;
        for (uint32_t path = 0u; path < route->path_count; ++path)
            blocked[route->path[path]] = 0u;
        used_converters[route->converter_nid] = 0u;
        if (ret != -ENODEV && ret != -EOVERFLOW) return ret;
        if (ret == -EOVERFLOW) saw_overflow = true;
        if (ret == -EOVERFLOW || last_error == -ENODEV)
            last_error = ret;
    }
    hda_codec_zero(route, sizeof(*route));
    return saw_overflow ? -EOVERFLOW : last_error;
}

/** @brief Discover and solve one bounded analog playback association.
 * @param codec Probed codec with CPU metadata allocator and immutable graph.
 * @param pin Seed output pin whose association is requested.
 * @param group Zero-initialized caller-owned group result.
 * @return 0 with feasible sorted members, graph/allocation errors, -ENODEV
 * for association zero/no route, -EINVAL for duplicate sequences/shared paths,
 * or -EOVERFLOW when member/channel limits are exceeded.
 *
 * @par Context
 * Task context during stream preparation; graph planning performs no verbs.
 * @par Ownership and locks/IRQ
 * Acquires one bounded logical codec gate, allocates CPU-only exact pages for
 * member snapshots, and transfers them to group on success. Failure frees the
 * exact acquired run. Group borrows codec and must be destroyed first.
 */
int hda_find_route_group(struct hda_codec *codec, uint8_t pin,
                         struct hda_route_group *group)
{
    if (!codec || !codec->probed || !codec->nodes || !codec->connections ||
        !group || !pin || group->codec || group->members ||
        group->members_phys || group->members_pages || group->member_count ||
        group->association || group->total_channels || group->active ||
        group->rollback_failed || group->rollback_error || group->reserved[0] ||
        group->reserved[1])
        return -EINVAL;
    int ret = hda_codec_route_gate_enter(codec);
    if (ret) return ret;
    struct hda_codec_node *seed = &codec->nodes[pin];
    if (!seed->present || seed->type != HDA_WIDGET_PIN ||
        (seed->widget_caps & HDA_WCAP_DIGITAL) ||
        !(seed->pin_caps & HDA_PINCAP_OUTPUT) ||
        (((seed->pin_default >> 30) & 3u) == 1u)) {
        ret = -ENODEV;
        goto done;
    }
    uint8_t association = (uint8_t)((seed->pin_default >> 4) & 0x0fu);
    if (!association) {
        ret = -ENODEV;
        goto done;
    }
    if (!codec->api || !codec->api->alloc_pages || !codec->api->free_pages) {
        ret = -EINVAL;
        goto done;
    }

    uint8_t sorted_pins[HDA_ROUTE_GROUP_MAX_MEMBERS];
    uint32_t member_count = 0u;
    uint8_t seed_afg = seed->afg_nid;
    uint32_t first_nid = association == 15u ? pin : 1u;
    uint32_t end_nid = association == 15u ? (uint32_t)pin + 1u :
                                               HDA_CODEC_MAX_NIDS;
    for (uint32_t nid = first_nid; nid < end_nid; ++nid) {
        struct hda_codec_node *candidate = &codec->nodes[nid];
        if (!candidate->present || candidate->type != HDA_WIDGET_PIN ||
            candidate->afg_nid != seed_afg ||
            (candidate->widget_caps & HDA_WCAP_DIGITAL) ||
            !(candidate->pin_caps & HDA_PINCAP_OUTPUT) ||
            (((candidate->pin_default >> 30) & 3u) == 1u) ||
            ((candidate->pin_default >> 4) & 0x0fu) != association)
            continue;
        if (member_count == HDA_ROUTE_GROUP_MAX_MEMBERS) {
            ret = -EOVERFLOW;
            goto done;
        }
        uint8_t sequence = (uint8_t)(candidate->pin_default & 0x0fu);
        uint32_t insert = 0u;
        while (insert < member_count) {
            uint8_t previous = sorted_pins[insert];
            uint8_t previous_sequence = (uint8_t)(
                codec->nodes[previous].pin_default & 0x0fu);
            if (previous_sequence == sequence) {
                ret = -EINVAL;
                goto done;
            }
            if (previous_sequence > sequence) break;
            ++insert;
        }
        for (uint32_t move = member_count; move > insert; --move)
            sorted_pins[move] = sorted_pins[move - 1u];
        sorted_pins[insert] = (uint8_t)nid;
        ++member_count;
    }
    if (!member_count) {
        ret = -ENODEV;
        goto done;
    }
    if (codec->route_groups == UINT32_MAX) {
        ret = -EOVERFLOW;
        goto done;
    }

    uint64_t bytes = (uint64_t)member_count * sizeof(struct hda_route);
    uint64_t pages64 = (bytes + HDA_CODEC_PAGE_BYTES - 1u) /
                       HDA_CODEC_PAGE_BYTES;
    if (!pages64 || pages64 > UINT32_MAX) {
        ret = -EOVERFLOW;
        goto done;
    }
    uint32_t pages = (uint32_t)pages64;
    uint64_t phys = codec->api->alloc_pages(pages);
    if (!phys) {
        ret = -ENOMEM;
        goto done;
    }
    struct hda_route *members = hda_codec_map_metadata_run(phys, pages);
    if (!members) {
        codec->api->free_pages(phys, pages);
        ret = -ERANGE;
        goto done;
    }
    hda_codec_zero(members, (uint64_t)pages * HDA_CODEC_PAGE_BYTES);

    uint8_t blocked[HDA_CODEC_MAX_NIDS];
    uint8_t used_converters[HDA_CODEC_MAX_NIDS];
    hda_codec_zero(blocked, sizeof(blocked));
    hda_codec_zero(used_converters, sizeof(used_converters));
    uint32_t search_budget = 4096u;
    ret = hda_route_group_solve(codec, sorted_pins, member_count, 0u, 0u,
                                members, blocked, used_converters,
                                &search_budget);
    uint32_t total_channels = 0u;
    if (!ret) {
        for (uint32_t i = 0u; i < member_count; ++i)
            total_channels += members[i].channel_count;
    }
    if (ret) {
        codec->api->free_pages(phys, pages);
        goto done;
    }
    group->codec = codec;
    group->members = members;
    group->members_phys = phys;
    group->members_pages = pages;
    group->member_count = (uint16_t)member_count;
    group->association = association;
    group->total_channels = (uint8_t)total_channels;
    ++codec->route_groups;
    ret = 0;

done:
    hda_codec_route_gate_leave(codec);
    return ret;
}

/** @brief Free an inactive route group's exact CPU metadata run.
 * @param group Caller-owned group to release.
 * @return 0 after the group reference and pages are released, -EBUSY while
 * any member is active, or -EINVAL for inconsistent ownership metadata.
 *
 * @par Context
 * Task teardown after route/group operations have stopped.
 * @par Ownership and locks/IRQ
 * Uses the codec logical route gate; frees only the exact group alloc_pages
 * run, decrements the codec lifetime reference, and zeroes group on success.
 */
int hda_route_group_destroy(struct hda_route_group *group)
{
    if (!group || !group->codec || !group->members || !group->members_phys ||
        !group->members_pages || !group->member_count)
        return -EINVAL;
    struct hda_codec *codec = group->codec;
    int ret = hda_codec_route_gate_enter(codec);
    if (ret) return ret;
    if (group->active || !codec->route_groups) {
        hda_codec_route_gate_leave(codec);
        return group->active ? -EBUSY : -EINVAL;
    }
    for (uint32_t i = 0u; i < group->member_count; ++i) {
        if (group->members[i].active) {
            hda_codec_route_gate_leave(codec);
            return -EBUSY;
        }
    }
    if (!codec->api || !codec->api->free_pages) {
        hda_codec_route_gate_leave(codec);
        return -EINVAL;
    }
    codec->api->free_pages(group->members_phys, group->members_pages);
    --codec->route_groups;
    hda_codec_zero(group, sizeof(*group));
    hda_codec_route_gate_leave(codec);
    return 0;
}

/** @brief Clear one converter's stream assignment and verify both zero values.
 * @param group Active route group whose codec metadata remains pinned.
 * @param route Converter route being cleared.
 * @return 0 after both zero readbacks, or the first transport/verification error.
 * Every verb is attempted after the first error so a transient transport fault
 * cannot hide the second readback. The dirty marker keeps the route retryable.
 *
 * @par Context
 * Task context while the codec route gate is held; no IRQ/service caller.
 * @par Ownership and locks/IRQ
 * Borrows group and route metadata under the route gate; the command boundary
 * serializes each verb and this helper does not alter interrupt state.
 */
static int hda_route_stream_clear_member(struct hda_route_group *group,
                                         struct hda_route *route)
{
    struct hda_codec *codec = group->codec;
    uint32_t response = 0u;
    int first_error = 0;
    int ret = hda_exec_verb(codec->controller, codec->cad, route->converter_nid,
                            HDA_VERB_SET_STREAM_CHANNEL, 0u, false, &response);
    if (ret && !first_error) first_error = ret;
    ret = hda_exec_verb(codec->controller, codec->cad, route->converter_nid,
                        HDA_VERB_SET_CONVERTER_FORMAT, 0u, true, &response);
    if (ret && !first_error) first_error = ret;
    ret = hda_exec_verb(codec->controller, codec->cad, route->converter_nid,
                        HDA_VERB_GET_STREAM_CHANNEL, 0u, false, &response);
    if (ret && !first_error) first_error = ret;
    else if (!ret && response != 0u && !first_error) first_error = -EIO;
    ret = hda_exec_verb(codec->controller, codec->cad, route->converter_nid,
                        HDA_VERB_GET_CONVERTER_FORMAT, 0u, false, &response);
    if (ret && !first_error) first_error = ret;
    else if (!ret && (response & 0xffffu) != 0u && !first_error) first_error = -EIO;
    route->stream_bind_dirty = first_error ? 1u : 0u;
    route->stream_bind_error = first_error;
    if (!first_error) {
        route->stream_bound = 0u;
        route->stream_tag = 0u;
        route->stream_format = 0u;
    }
    return first_error;
}

/** @brief Bind and verify one complete stream format on every route converter.
 * @param group Active route group whose codec metadata remains pinned.
 * @param tag HDA stream tag, or zero to release stream/channel and format.
 * @param format Complete HDA format word including the full-stream channel count.
 * @return 0 after capability checks and readback, or a negative transport/state errno.
 *
 * @par Context
 * Task context with the codec route gate; no IRQ/service caller is permitted.
 * @par Ownership and locks/IRQ
 * Caller retains the active route group; the route gate serializes codec verbs
 * and no interrupt state is changed.
 */
int hda_route_group_stream_bind(struct hda_route_group *group, uint8_t tag,
                                uint16_t format)
{
    if (!group || !group->codec || !group->members || !group->active ||
        (!!tag != !!format)) return -EINVAL;
    struct hda_codec *codec = group->codec;
    int ret = hda_codec_route_gate_enter(codec);
    if (ret) return ret;
    uint32_t touched_count = 0u;
    if (!tag) {
        int first_error = 0;
        for (uint32_t i = 0; i < group->member_count; ++i) {
            int clear_ret = hda_route_stream_clear_member(group,
                                                           &group->members[i]);
            if (clear_ret && !first_error) first_error = clear_ret;
        }
        ret = first_error;
        goto done;
    }
    uint32_t rate = (format & 0x4000u) ? 44100u : 48000u;
    uint32_t rate_bit = 1u << (rate == 44100u ? 5u : 6u);
    uint32_t width = (format >> 4) & 7u;
    if (width > 4u) { ret = -EINVAL; goto done; }
    uint32_t precision_bit = 1u << (16u + width);
    for (uint32_t i = 0; i < group->member_count; ++i) {
        struct hda_route *route = &group->members[i];
        uint32_t channels = (format & 0x0fu) + 1u;
        if (route->channel_start > 15u || !route->channel_count ||
            (uint32_t)route->channel_start + route->channel_count > channels) {
            ret = -EINVAL;
            goto done;
        }
        if (route->stream_bind_dirty) {
            ret = -EBUSY;
            goto done;
        }
        const struct hda_codec_node *node = &codec->nodes[route->converter_nid];
        if (!(node->pcm_rates & rate_bit) || !(node->pcm_rates & precision_bit) ||
            !(node->stream_formats & 1u)) {
            ret = -EOPNOTSUPP;
            goto done;
        }
    }
    for (uint32_t i = 0; i < group->member_count; ++i) {
        struct hda_route *route = &group->members[i];
        uint32_t response = 0u;
        ++touched_count;
        route->stream_bind_dirty = 1u;
        ret = hda_exec_verb(codec->controller, codec->cad, route->converter_nid,
                            HDA_VERB_SET_STREAM_CHANNEL,
                            (uint16_t)(((uint16_t)tag << 4) | route->channel_start),
                            false, &response);
        if (ret) goto done;
        ret = hda_exec_verb(codec->controller, codec->cad, route->converter_nid,
                            HDA_VERB_SET_CONVERTER_FORMAT, format, true, &response);
        if (ret) goto done;
        ret = hda_exec_verb(codec->controller, codec->cad, route->converter_nid,
                            HDA_VERB_GET_STREAM_CHANNEL, 0u, false, &response);
        if (ret || ((response & 0xffu) != (((uint32_t)tag << 4) | route->channel_start))) {
            ret = ret ? ret : -EIO;
            goto done;
        }
        ret = hda_exec_verb(codec->controller, codec->cad, route->converter_nid,
                            HDA_VERB_GET_CONVERTER_FORMAT, 0u, false, &response);
        if (ret || ((response & 0xffffu) != format)) {
            ret = ret ? ret : -EIO;
            goto done;
        }
        route->stream_bind_dirty = 0u;
        route->stream_bind_error = 0;
    }
    ret = 0;
done:
    if (ret && tag) {
        int cleanup_error = 0;
        for (uint32_t i = 0; i < touched_count; ++i) {
            int clear_ret = hda_route_stream_clear_member(group,
                                                           &group->members[i]);
            if (clear_ret && !cleanup_error) cleanup_error = clear_ret;
        }
        if (!ret) ret = cleanup_error;
    }
    hda_codec_route_gate_leave(codec);
    return ret;
}

/** @brief Read one channel of a codec input/output amplifier using the GET index field.
 * @param codec Probed cache borrowing the live controller.
 * @param nid Widget with the amplifier.
 * @param input True for an input amplifier; false for output.
 * @param index Four-bit GET input-amplifier index; output amplifiers use zero.
 * @param right True for right channel; false for left.
 * @param out_value Receives raw gain/mute response byte.
 * @return 0 or transport error; -ERANGE if the index cannot be encoded.
 *
 * @par Context
 * Task context during serialized route configuration, outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows codec/controller state; the command flight owns its short lock, never the whole route transaction.
 */
static int hda_codec_read_amp(struct hda_codec *codec, uint8_t nid, bool input,
                              uint8_t index, bool right, uint16_t *out_value)
{
    if (!codec || !codec->nodes || !out_value)
        return -EINVAL;
    if (index > 15u) return -ERANGE;
    if (right && !(codec->nodes[nid].widget_caps & HDA_WCAP_STEREO)) {
        *out_value = 0u;
        return 0;
    }
    uint16_t payload = (!input ? HDA_AMP_GET_OUTPUT : 0u) |
        (right ? 0u : HDA_AMP_GET_LEFT) | (input ? index : 0u);
    uint32_t response = 0u;
    int ret = hda_exec_verb(codec->controller, codec->cad, nid,
                            HDA_VERB_GET_AMP, payload, true, &response);
    if (!ret) *out_value = (uint16_t)(response & 0xffu);
    return ret;
}

/** @brief Set one amplifier channel and verify the device's gain/mute readback.
 * @param codec Probed cache borrowing the live controller.
 * @param nid Widget with the amplifier.
 * @param input True for an input amplifier; false for output.
 * @param index Four-bit SET input-amplifier index; output amplifiers use zero.
 * @param right True for right channel; false for left.
 * @param value Exact saved or desired gain/mute byte.
 * @param amp_caps Effective inherited or local amplifier capabilities.
 * @return 0 after matching readback, -ERANGE for unsupported index/gain, -EIO for mismatch, or transport errno.
 *
 * @par Context
 * Task context while the per-codec route transaction gate is owned; hda_exec_verb waits outside all route spinlocks.
 * @par Ownership and locks/IRQ
 * Does not retain or transfer amplifier ownership; caller's route snapshot remains the rollback owner.
 */
static int hda_codec_write_amp(struct hda_codec *codec, uint8_t nid, bool input,
                               uint8_t index, bool right, uint16_t value,
                               uint32_t amp_caps)
{
    if (index > 15u) return -ERANGE;
    if (!codec || !codec->nodes) return -EINVAL;
    if (!input && index) return -ERANGE;
    if (right && !(codec->nodes[nid].widget_caps & HDA_WCAP_STEREO))
        return 0;
    uint32_t max_gain = (amp_caps & HDA_AMP_CAP_NUM_STEPS_MASK) >>
                        HDA_AMP_CAP_NUM_STEPS_SHIFT;
    if ((value & HDA_AMP_GAIN_MASK) > max_gain) return -ERANGE;
    uint16_t channel = right ? HDA_AMP_SET_RIGHT : HDA_AMP_SET_LEFT;
    uint16_t index_payload = input ?
        (uint16_t)((uint16_t)index << HDA_AMP_SET_INDEX_SHIFT) : (uint16_t)0u;
    uint16_t payload = (uint16_t)(
        (input ? HDA_AMP_SET_INPUT : HDA_AMP_SET_OUTPUT) | channel |
        index_payload | (value & (HDA_AMP_SET_MUTE | HDA_AMP_GAIN_MASK)));
    uint32_t ignored = 0u;
    int ret = hda_exec_verb(codec->controller, codec->cad, nid,
                            HDA_VERB_SET_AMP, payload, true, &ignored);
    if (ret) return ret;
    uint16_t actual = 0u;
    ret = hda_codec_read_amp(codec, nid, input, index, right, &actual);
    if (ret) return ret;
    uint16_t compare_mask = HDA_AMP_GAIN_MASK;
    if (amp_caps & HDA_AMP_CAP_MUTE) compare_mask |= HDA_AMP_SET_MUTE;
    if ((actual & compare_mask) != (value & compare_mask)) return -EIO;
    return 0;
}

/** @brief Read one task-context mixer amplifier channel with exact wire encoding.
 * @param codec Codec cache with a live controller transport.
 * @param nid Amplifier widget NID.
 * @param input Select input amplifier when true, output when false.
 * @param index Input amplifier index, or zero for output.
 * @param right Select the right channel when true.
 * @param out_value Receives the raw gain/mute value after a checked verb.
 * @return 0 or the original validation/transport error.
 * @par Context Task context while the caller owns the codec control gate.
 * @par Ownership and locks/IRQ Caller owns the output and codec lifetime; this
 * wrapper does not hold a lock across the synchronous verb.
 */
int hda_codec_control_read_amp(struct hda_codec *codec, uint8_t nid, bool input,
                               uint8_t index, bool right, uint16_t *out_value)
{
    return hda_codec_read_amp(codec, nid, input, index, right, out_value);
}

/** @brief Write one mixer amplifier channel and require exact hardware readback.
 * @param codec Codec cache with a live controller transport.
 * @param nid Amplifier widget NID.
 * @param input Select input amplifier when true, output when false.
 * @param index Input amplifier index, or zero for output.
 * @param right Select the right channel when true.
 * @param value Raw gain/mute value to write.
 * @param amp_caps Effective amplifier capability bits used for validation.
 * @return 0 after readback, or the original validation/transport/readback error.
 * @par Context Task context while the caller owns the codec control gate.
 * @par Ownership and locks/IRQ Caller owns the codec lifetime; no lock spans
 * the synchronous verb and IRQ progress remains enabled.
 */
int hda_codec_control_write_amp(struct hda_codec *codec, uint8_t nid, bool input,
                                uint8_t index, bool right, uint16_t value,
                                uint32_t amp_caps)
{
    return hda_codec_write_amp(codec, nid, input, index, right, value, amp_caps);
}

/** @brief Read checked PS_Set/PS_Act fields and reject PS_Error.
 * @param codec Probed codec with live controller.
 * @param nid Function Group or power-capable widget NID.
 * @param out_set Receives PS_Set bits 3:0.
 * @param out_act Receives PS_Act bits 7:4.
 * @return 0 or transport error / -EIO when PS_Error is set.
 * @par Context
 * Task route transaction outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows controller state; holds no lock over the verb wait.
 */
static int hda_codec_read_power_status(struct hda_codec *codec, uint8_t nid,
                                       uint8_t *out_set, uint8_t *out_act);

/** @brief Set a power request and await matching actual state with a deadline.
 * @param codec Probed codec with live controller task clock.
 * @param nid Function Group or power-capable widget NID.
 * @param target Requested state in bits 3:0.
 * @return 0, transport errno, -EIO for PS_Error/mismatched Set, or bounded timeout.
 * @par Context
 * Task context only, outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Route may own only its logical gate; no spinlock spans verbs/sleep. The H1
 * wrapper permits IRQ progress and restores entry IF; not IRQ/service callable.
 */
static int hda_codec_set_power_state(struct hda_codec *codec, uint8_t nid,
                                     uint8_t target);

/** @brief Save all control values that a route may change before its first write.
 * @param codec Probed codec with a live serialized verb transport.
 * @param route Resolved path whose caller-owned snapshot arrays are filled.
 * @return 0 or the first transport, invalid-state, or unencodable-amp error.
 *
 * @par Context
 * Task context while the codec route gate is held, outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Stores only values in caller-owned route metadata; no route writes occur until every snapshot succeeds.
 */
static int hda_route_take_snapshot(struct hda_codec *codec,
                                   struct hda_route *route)
{
    uint8_t afg_nid = codec->nodes[route->pin_nid].afg_nid;
    struct hda_codec_node *afg = &codec->nodes[afg_nid];
    if (!afg->present || afg->type != HDA_WIDGET_FUNCTION_GROUP)
        return -EINVAL;
    route->afg_lease_acquired = 0u;
    route->control_lease_acquired = 0u;
    route->saved_afg_power = 0u;
    uint8_t afg_set = 0u;
    uint8_t afg_act = 0u;
    int ret = hda_codec_read_power_status(codec, afg_nid, &afg_set,
                                          &afg_act);
    if (ret) return ret;
    (void)afg_act;
    if (codec->afg_lease_count[afg_nid]) {
        if (!codec->afg_lease_valid[afg_nid]) return -EIO;
        route->saved_afg_power = codec->afg_lease_saved_power[afg_nid];
    } else {
        route->saved_afg_power = afg_set;
    }

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        route->saved_flags[i] = 0u;
        route->changed_flags[i] = 0u;
        uint8_t nid = route->path[i];
        struct hda_codec_node *node = &codec->nodes[nid];
        uint32_t response = 0u;
        ret = 0;
        if (node->widget_caps & HDA_WCAP_POWER) {
            uint8_t power_set = 0u;
            uint8_t power_act = 0u;
            ret = hda_codec_read_power_status(codec, nid, &power_set,
                                              &power_act);
            if (ret) return ret;
            (void)power_act;
            route->saved_power[i] = power_set;
            route->saved_flags[i] |= HDA_ROUTE_CHANGED_POWER;
        }
        if (node->connection_count > 1u &&
            (node->type == HDA_WIDGET_SELECTOR ||
             node->type == HDA_WIDGET_PIN ||
             node->type == HDA_WIDGET_AUDIO_INPUT)) {
            ret = hda_exec_verb(codec->controller, codec->cad, nid,
                                HDA_VERB_GET_CONNECTION_SELECT, 0u, false,
                                &response);
            if (ret) return ret;
            if (response >= node->connection_count) return -EIO;
            route->saved_selector[i] = (uint8_t)response;
            route->saved_flags[i] |= HDA_ROUTE_CHANGED_SELECTOR;
        }
        if (node->type == HDA_WIDGET_PIN) {
            ret = hda_exec_verb(codec->controller, codec->cad, nid,
                                HDA_VERB_GET_PIN_CONTROL, 0u, false, &response);
            if (ret) return ret;
            route->saved_pin_control[i] = (uint8_t)response;
            route->saved_flags[i] |= HDA_ROUTE_CHANGED_PIN;
            if (node->pin_caps & HDA_PINCAP_EAPD) {
                ret = hda_exec_verb(codec->controller, codec->cad, nid,
                                    HDA_VERB_GET_EAPD, 0u, false, &response);
                if (ret) return ret;
                route->saved_eapd[i] = (uint8_t)response;
                route->saved_flags[i] |= HDA_ROUTE_CHANGED_EAPD;
            }
        }

        uint8_t input_index = 0u;
        if (node->connection_count && route->connection_index[i] != 0xffu)
            input_index = route->connection_index[i];
        if (node->widget_caps & HDA_WCAP_IN_AMP) {
            if (input_index > 15u) return -ERANGE;
            ret = hda_codec_read_amp(codec, nid, true, input_index, false,
                                     &route->saved_input_amp[i][0]);
            if (ret) return ret;
            route->saved_input_amp[i][1] = 0u;
            if (node->widget_caps & HDA_WCAP_STEREO) {
                ret = hda_codec_read_amp(codec, nid, true, input_index, true,
                                         &route->saved_input_amp[i][1]);
                if (ret) return ret;
            }
            uint32_t max_gain = (node->amp_input_caps & HDA_AMP_CAP_NUM_STEPS_MASK) >>
                                HDA_AMP_CAP_NUM_STEPS_SHIFT;
            if ((route->saved_input_amp[i][0] & HDA_AMP_GAIN_MASK) > max_gain ||
                ((node->widget_caps & HDA_WCAP_STEREO) &&
                 (route->saved_input_amp[i][1] & HDA_AMP_GAIN_MASK) > max_gain))
                return -ERANGE;
            route->saved_flags[i] |= HDA_ROUTE_CHANGED_INPUT_AMP;
        }
        if (node->widget_caps & HDA_WCAP_OUT_AMP) {
            ret = hda_codec_read_amp(codec, nid, false, 0u, false,
                                     &route->saved_output_amp[i][0]);
            if (ret) return ret;
            route->saved_output_amp[i][1] = 0u;
            if (node->widget_caps & HDA_WCAP_STEREO) {
                ret = hda_codec_read_amp(codec, nid, false, 0u, true,
                                         &route->saved_output_amp[i][1]);
                if (ret) return ret;
            }
            uint32_t max_gain = (node->amp_output_caps & HDA_AMP_CAP_NUM_STEPS_MASK) >>
                                HDA_AMP_CAP_NUM_STEPS_SHIFT;
            if ((route->saved_output_amp[i][0] & HDA_AMP_GAIN_MASK) > max_gain ||
                ((node->widget_caps & HDA_WCAP_STEREO) &&
                 (route->saved_output_amp[i][1] & HDA_AMP_GAIN_MASK) > max_gain))
                return -ERANGE;
            route->saved_flags[i] |= HDA_ROUTE_CHANGED_OUTPUT_AMP;
        }
    }
    route->snapshots_valid = 1u;
    return 0;
}

/** @brief Execute one setter and require an exact masked device readback.
 * @param codec Probed cache borrowing the live controller.
 * @param nid Node receiving the control update.
 * @param set_verb Long-form SET verb identifier.
 * @param get_verb Long-form GET verb identifier used to read back state.
 * @param value Value sent as the SET payload.
 * @param mask Meaningful bits that must match in the GET response.
 * @return 0 after readback, the original transport error, or -EIO on mismatch.
 *
 * @par Context
 * Task context while the route gate is owned, outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows codec and route hardware state; no spinlock is retained during either verb.
 */
static int hda_codec_set_checked(struct hda_codec *codec, uint8_t nid,
                                 uint16_t set_verb, uint16_t get_verb,
                                 uint16_t value, uint32_t mask)
{
    uint32_t ignored = 0u;
    int ret = hda_exec_verb(codec->controller, codec->cad, nid, set_verb,
                            value, false, &ignored);
    if (ret) return ret;
    uint32_t actual = 0u;
    ret = hda_exec_verb(codec->controller, codec->cad, nid, get_verb, 0u,
                        false, &actual);
    if (ret) return ret;
    return (actual & mask) == (value & mask) ? 0 : -EIO;
}

/** @brief Read checked HDA power status without conflating Act and Set fields.
 * @param codec Probed codec borrowing the initialized controller.
 * @param nid AFG or power-capable widget node.
 * @param out_set Receives PS_Set from response bits 3:0.
 * @param out_act Receives PS_Act from response bits 7:4.
 * @return 0 with both fields or -EIO when the device reports PS_Error.
 *
 * @par Context
 * Task route transaction outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows controller/codec state; no lock is held over the synchronous verb.
 */
static int hda_codec_read_power_status(struct hda_codec *codec, uint8_t nid,
                                       uint8_t *out_set, uint8_t *out_act)
{
    if (!codec || !codec->controller || !out_set || !out_act) return -EINVAL;
    uint32_t response = 0u;
    int ret = hda_exec_verb(codec->controller, codec->cad, nid,
                            HDA_VERB_GET_POWER_STATE, 0u, false, &response);
    if (ret) return ret;
    if (response & HDA_POWER_ERROR) return -EIO;
    *out_set = (uint8_t)(response & HDA_POWER_STATE_MASK);
    *out_act = (uint8_t)((response >> HDA_POWER_ACT_SHIFT) &
                         HDA_POWER_STATE_MASK);
    return 0;
}

/** @brief Request a power state and wait boundedly for its actual state.
 * @param codec Probed codec borrowing a live controller and task clock.
 * @param nid AFG or power-capable widget to transition.
 * @param target Requested four-bit HDA power state.
 * @return 0 when PS_Set and PS_Act match target without PS_Error, a transport
 * error, -EIO for rejected/mismatched PS_Set, -ETIMEDOUT for delayed Act, or
 * -ENOTSUP if an asynchronous transition lacks a task clock/sleep service.
 *
 * @par Context
 * Task context only while the codec/controller and H1 clock services are live.
 * @par Ownership and locks/IRQ
 * The codec route gate may remain logically owned but no spinlock is held over
 * verbs or sleep. The private controller wait wrapper permits IRQ progress and
 * restores entry IF; never call from IRQ/service/global execution callbacks.
 */
static int hda_codec_set_power_state(struct hda_codec *codec, uint8_t nid,
                                     uint8_t target)
{
    if (!codec || !codec->controller || target > HDA_POWER_STATE_MASK)
        return -EINVAL;
    const struct reliefos_driver_kernel_api *api = codec->controller->api;
    uint8_t current_set = 0u;
    uint8_t current_act = 0u;
    int ret = hda_codec_read_power_status(codec, nid, &current_set,
                                          &current_act);
    if (ret) return ret;
    if (current_set != target) {
        uint32_t ignored = 0u;
        ret = hda_exec_verb(codec->controller, codec->cad, nid,
                            HDA_VERB_SET_POWER_STATE, target, false, &ignored);
        if (ret) return ret;
        /* A SET response does not report PS_Error or the applied state. */
        ret = hda_codec_read_power_status(codec, nid, &current_set,
                                          &current_act);
        if (ret) return ret;
        if (current_set != target) return -EIO;
        if (current_act == target) return 0;
    }

    if (current_act == target) return 0;
    if (!api || !api->ticks || !api->sleep_ms) return -EOPNOTSUPP;
    uint64_t start = api->ticks();
    for (uint32_t poll = 0u; poll < HDA_POWER_TRANSITION_MAX_POLLS; ++poll) {
        ret = hda_codec_read_power_status(codec, nid, &current_set,
                                          &current_act);
        if (ret) return ret;
        if (current_set != target) return -EIO;
        if (current_act == target) return 0;
        if (api->ticks() - start >= HDA_POWER_TRANSITION_TIMEOUT_TICKS)
            return -ETIMEDOUT;
        hda_controller_sleep_ms(codec->controller, 1u);
    }
    return -ETIMEDOUT;
}

/** @brief Acquire exclusive signal nodes and one shared AFG power lease.
 * @param codec Probed cache whose route transaction gate is held.
 * @param route Snapshot-complete path acquiring both ownership classes.
 * @return 0 after ownership is recorded, -EBUSY for a signal-node conflict,
 * -EOVERFLOW for a saturated AFG count, or -EIO for inconsistent lease state.
 *
 * @par Context
 * Task context before the first route write.
 * @par Ownership and locks/IRQ
 * Mutates only codec/route metadata under the bounded logical route gate; no
 * verb, wait, allocation, or spinlock is involved.
 */
static int hda_route_acquire_leases(struct hda_codec *codec,
                                    struct hda_route *route)
{
    int ret = hda_route_check_control_lease(codec, route);
    if (ret) return ret;
    uint8_t afg_nid = codec->nodes[route->pin_nid].afg_nid;
    uint32_t count = codec->afg_lease_count[afg_nid];
    if (count == UINT32_MAX) return -EOVERFLOW;
    if (count && !codec->afg_lease_valid[afg_nid]) return -EIO;
    if (!count) {
        codec->afg_lease_saved_power[afg_nid] = route->saved_afg_power;
        codec->afg_lease_valid[afg_nid] = 1u;
    } else {
        route->saved_afg_power = codec->afg_lease_saved_power[afg_nid];
    }
    codec->afg_lease_count[afg_nid] = count + 1u;
    route->afg_lease_acquired = 1u;
    for (uint32_t i = 0u; i < route->path_count; ++i)
        codec->route_node_owner[route->path[i]] = 1u;
    route->control_lease_acquired = 1u;
    return 0;
}

/** @brief Release signal nodes and safely drop one shared AFG lease.
 * @param codec Probed cache whose route transaction gate is held.
 * @param route Path releasing ownership after control restoration.
 * @return 0 after release; a failed last-owner power restore retains every
 * ownership record and the original state for a later disable retry.
 *
 * @par Context
 * Task context after local route controls have been restored.
 * @par Ownership and locks/IRQ
 * The logical gate protects metadata while power transition helpers wait
 * without a spinlock. Caller keeps controller/API services alive for retry.
 */
static int hda_route_release_leases(struct hda_codec *codec,
                                    struct hda_route *route)
{
    if (!route->afg_lease_acquired || !route->control_lease_acquired)
        return -EINVAL;
    uint8_t afg_nid = codec->nodes[route->pin_nid].afg_nid;
    uint32_t count = codec->afg_lease_count[afg_nid];
    if (!count || !codec->afg_lease_valid[afg_nid]) return -EIO;
    for (uint32_t i = 0u; i < route->path_count; ++i)
        if (!codec->route_node_owner[route->path[i]]) return -EIO;

    if (count == 1u) {
        int ret = hda_codec_set_power_state(codec, afg_nid,
                            codec->afg_lease_saved_power[afg_nid]);
        if (ret) return ret;
        codec->afg_lease_count[afg_nid] = 0u;
        codec->afg_lease_valid[afg_nid] = 0u;
        codec->afg_lease_saved_power[afg_nid] = 0u;
    } else {
        codec->afg_lease_count[afg_nid] = count - 1u;
    }
    for (uint32_t i = 0u; i < route->path_count; ++i)
        codec->route_node_owner[route->path[i]] = 0u;
    route->afg_lease_acquired = 0u;
    route->control_lease_acquired = 0u;
    return 0;
}

/** @brief Mute one route amplifier while preserving its prior gain value.
 * @param codec Probed cache borrowing the live controller.
 * @param route Owner of exact pre-route amp values and changed flags.
 * @param path_index Index of the amplifier's widget within route->path.
 * @param input Select input versus output amplifier.
 * @param right Select right versus left channel.
 * @return 0 after mute readback, or when mute is unsupported; otherwise the
 * first transport/readback error.
 *
 * @par Context
 * Task route transaction; uses the HDA GET low-nibble index and SET bits 11:8 index encodings.
 * @par Ownership and locks/IRQ
 * Mutates only one hardware amp and the corresponding caller-owned changed bit; no lock spans the verb wait.
 */
static int hda_route_mute_amp(struct hda_codec *codec, struct hda_route *route,
                              uint32_t path_index, bool input, bool right)
{
    uint8_t nid = route->path[path_index];
    struct hda_codec_node *node = &codec->nodes[nid];
    uint8_t index = 0u;
    uint16_t prior = 0u;
    uint32_t caps;
    if (input) {
        caps = node->amp_input_caps;
        if (route->connection_index[path_index] != 0xffu)
            index = route->connection_index[path_index];
        prior = route->saved_input_amp[path_index][right ? 1u : 0u];
    } else {
        caps = node->amp_output_caps;
        prior = route->saved_output_amp[path_index][right ? 1u : 0u];
    }
    if (!(caps & HDA_AMP_CAP_MUTE)) return 0;
    route->changed_flags[path_index] |= input ? HDA_ROUTE_CHANGED_INPUT_AMP :
                                                 HDA_ROUTE_CHANGED_OUTPUT_AMP;
    return hda_codec_write_amp(codec, nid, input, index, right,
                               (uint16_t)((prior & HDA_AMP_GAIN_MASK) |
                                          HDA_AMP_SET_MUTE), caps);
}

/** @brief Identify QEMU's known immutable input/output-only analog pins.
 * @param codec Probed codec identity.
 * @param node Borrowed pin widget metadata.
 * @return True only for six QEMU codec IDs and input/output-only pin caps.
 * Task metadata lookup; no hardware write, allocation, or ownership change.
 */
static bool hda_codec_fixed_qemu_pin(const struct hda_codec *codec,
                                     const struct hda_codec_node *node)
{
    if (!codec || !node || node->type != HDA_WIDGET_PIN ||
        (node->pin_caps != HDA_PINCAP_OUTPUT && node->pin_caps != HDA_PINCAP_INPUT))
        return false;
    switch (codec->vendor_id) {
    case 0x1af40011u: case 0x1af40012u:
    case 0x1af40021u: case 0x1af40022u:
    case 0x1af40031u: case 0x1af40032u:
        return true;
    default:
        return false;
    }
}

/** @brief Disable mutable route pins and verify known fixed QEMU pin state.
 * @param codec Probed codec borrowing the live controller.
 * @param route Snapshot-complete route whose pin controls are owned by caller.
 * @return 0 after every pin gate reads back, otherwise the first error.
 *
 * @par Context
 * Task route transaction before topology writes and before restoration writes.
 * @par Ownership and locks/IRQ
 * Records pending pin restoration in route->changed_flags before each checked
 * verb; no lock spans synchronous controller I/O.
 */
static int hda_route_gate_pins(struct hda_codec *codec,
                               struct hda_route *route)
{
    for (uint32_t i = 0u; i < route->path_count; ++i) {
        struct hda_codec_node *node = &codec->nodes[route->path[i]];
        if (node->type != HDA_WIDGET_PIN) continue;
        if (hda_codec_fixed_qemu_pin(codec, node)) {
            /* QEMU GET returns a descriptor constant; SET does not change it.
             * Gate the real converter amplifiers instead, and still verify
             * that this fixed pin already carries the requested direction. */
            uint32_t actual = 0u;
            uint8_t expected = route->direction == HDA_ROUTE_PLAYBACK ?
                                HDA_PINCTL_OUTPUT_ENABLE : HDA_PINCTL_INPUT_ENABLE;
            int ret = hda_exec_verb(codec->controller, codec->cad, route->path[i],
                                    HDA_VERB_GET_PIN_CONTROL, 0u, false, &actual);
            if (ret) return ret;
            if (route->saved_pin_control[i] != expected || (actual & 0xffu) != expected)
                return -EIO;
            continue;
        }
        uint8_t gate = route->saved_pin_control[i] &
            (uint8_t)~(HDA_PINCTL_HP_ENABLE | HDA_PINCTL_OUTPUT_ENABLE |
                       HDA_PINCTL_INPUT_ENABLE);
        route->changed_flags[i] |= HDA_ROUTE_CHANGED_PIN;
        int ret = hda_codec_set_checked(codec, route->path[i],
                                        HDA_VERB_SET_PIN_CONTROL,
                                        HDA_VERB_GET_PIN_CONTROL, gate, 0xffu);
        if (ret) return ret;
    }
    return 0;
}

/** @brief Restore one amplifier channel to its exact saved gain and mute state.
 * @param codec Probed cache borrowing the live controller.
 * @param route Caller-owned snapshot with a pending changed bit.
 * @param path_index Index of the amplifier widget within the path.
 * @param input Select input versus output amplifier.
 * @param right Select right versus left channel.
 * @return 0 after matching readback or transport/range error.
 *
 * @par Context
 * Task route rollback or route stop while the codec transaction gate is owned.
 * @par Ownership and locks/IRQ
 * Reads only caller-owned snapshot values and updates no ownership; no spinlock is held across verbs.
 */
static int hda_route_restore_amp(struct hda_codec *codec, struct hda_route *route,
                                 uint32_t path_index, bool input, bool right)
{
    uint8_t nid = route->path[path_index];
    struct hda_codec_node *node = &codec->nodes[nid];
    uint8_t index = 0u;
    uint16_t prior;
    uint32_t caps;
    if (input) {
        caps = node->amp_input_caps;
        if (route->connection_index[path_index] != 0xffu)
            index = route->connection_index[path_index];
        prior = route->saved_input_amp[path_index][right ? 1u : 0u];
    } else {
        caps = node->amp_output_caps;
        prior = route->saved_output_amp[path_index][right ? 1u : 0u];
    }
    return hda_codec_write_amp(codec, nid, input, index, right, prior, caps);
}

/** @brief Apply D0, graph selectors/mixer input amps, pins, and EAPD in safe order.
 * @param codec Probed codec borrowing the live controller.
 * @param route Resolved path with complete pre-write snapshots.
 * @return 0 when every setter reads back, or the first transport/readback error.
 *
 * @par Context
 * Task context while the per-codec logical route gate is owned; converter format/tag remains the prepared H4 stream's responsibility.
 * @par Ownership and locks/IRQ
 * Route storage owns rollback values; hardware controls remain codec-owned. No route-wide spinlock crosses synchronous verb waits.
 */
static int hda_route_apply_controls(struct hda_codec *codec,
                                    struct hda_route *route)
{
    int ret;
    ret = hda_route_gate_pins(codec, route);
    if (ret) return ret;

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        if (route->saved_flags[i] & HDA_ROUTE_CHANGED_INPUT_AMP) {
            ret = hda_route_mute_amp(codec, route, i, true, false);
            if (ret) return ret;
            ret = hda_route_mute_amp(codec, route, i, true, true);
            if (ret) return ret;
        }
        if (route->saved_flags[i] & HDA_ROUTE_CHANGED_OUTPUT_AMP) {
            ret = hda_route_mute_amp(codec, route, i, false, false);
            if (ret) return ret;
            ret = hda_route_mute_amp(codec, route, i, false, true);
            if (ret) return ret;
        }
    }

    uint8_t afg_nid = codec->nodes[route->pin_nid].afg_nid;
    ret = hda_codec_set_power_state(codec, afg_nid, 0u);
    if (ret) return ret;

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        if (!(route->saved_flags[i] & HDA_ROUTE_CHANGED_POWER)) continue;
        if (route->saved_power[i] != 0u)
            route->changed_flags[i] |= HDA_ROUTE_CHANGED_POWER;
        ret = hda_codec_set_power_state(codec, route->path[i], 0u);
        if (ret) return ret;
    }

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        if (!(route->saved_flags[i] & HDA_ROUTE_CHANGED_SELECTOR)) continue;
        struct hda_codec_node *node = &codec->nodes[route->path[i]];
        uint8_t selected = route->connection_index[i];
        if (selected >= node->connection_count) return -EINVAL;
        if (route->saved_selector[i] == selected) continue;
        route->changed_flags[i] |= HDA_ROUTE_CHANGED_SELECTOR;
        ret = hda_codec_set_checked(codec, route->path[i],
                                    HDA_VERB_SET_CONNECTION_SELECT,
                                    HDA_VERB_GET_CONNECTION_SELECT,
                                    selected, 0xffu);
        if (ret) return ret;
    }

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        struct hda_codec_node *node = &codec->nodes[route->path[i]];
        if (node->type != HDA_WIDGET_PIN ||
            !(route->saved_flags[i] & HDA_ROUTE_CHANGED_PIN)) continue;
        uint8_t control = route->saved_pin_control[i];
        control &= (uint8_t)~(HDA_PINCTL_HP_ENABLE | HDA_PINCTL_OUTPUT_ENABLE |
                              HDA_PINCTL_INPUT_ENABLE | HDA_PINCTL_VREF_MASK);
        if (route->direction == HDA_ROUTE_PLAYBACK) {
            control |= HDA_PINCTL_OUTPUT_ENABLE;
            uint8_t device = (uint8_t)((node->pin_default >> 20) & 0x0fu);
            if (device == 2u) control |= HDA_PINCTL_HP_ENABLE;
        } else {
            control |= HDA_PINCTL_INPUT_ENABLE;
            if (node->pin_caps & HDA_PINCAP_VREF_80)
                control |= HDA_PINCTL_VREF_80;
        }
        if (hda_codec_fixed_qemu_pin(codec, node)) {
            if (control != route->saved_pin_control[i]) return -EOPNOTSUPP;
            continue;
        }
        route->changed_flags[i] |= HDA_ROUTE_CHANGED_PIN;
        ret = hda_codec_set_checked(codec, route->path[i],
                                    HDA_VERB_SET_PIN_CONTROL,
                                    HDA_VERB_GET_PIN_CONTROL, control, 0xffu);
        if (ret) return ret;
        if (route->direction == HDA_ROUTE_PLAYBACK &&
            (node->pin_caps & HDA_PINCAP_EAPD) &&
            (route->saved_flags[i] & HDA_ROUTE_CHANGED_EAPD)) {
            uint8_t eapd = route->saved_eapd[i] | 2u;
            if (eapd != route->saved_eapd[i]) {
                route->changed_flags[i] |= HDA_ROUTE_CHANGED_EAPD;
                ret = hda_codec_set_checked(codec, route->path[i],
                                            HDA_VERB_SET_EAPD,
                                            HDA_VERB_GET_EAPD, eapd, 0xffu);
                if (ret) return ret;
            }
        }
    }

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        if (route->saved_flags[i] & HDA_ROUTE_CHANGED_INPUT_AMP) {
            for (uint32_t channel = 0u; channel < 2u; ++channel) {
                route->changed_flags[i] |= HDA_ROUTE_CHANGED_INPUT_AMP;
                uint32_t caps = codec->nodes[route->path[i]].amp_input_caps;
                uint16_t zero_db = (uint16_t)(caps &
                                               HDA_AMP_CAP_OFFSET_MASK);
                ret = hda_codec_write_amp(codec, route->path[i], true,
                    route->connection_index[i] == 0xffu ? 0u :
                        route->connection_index[i], channel != 0u,
                    zero_db, caps);
                if (ret) return ret;
            }
        }
        if (route->saved_flags[i] & HDA_ROUTE_CHANGED_OUTPUT_AMP) {
            for (uint32_t channel = 0u; channel < 2u; ++channel) {
                route->changed_flags[i] |= HDA_ROUTE_CHANGED_OUTPUT_AMP;
                uint32_t caps = codec->nodes[route->path[i]].amp_output_caps;
                uint16_t zero_db = (uint16_t)(caps &
                                               HDA_AMP_CAP_OFFSET_MASK);
                ret = hda_codec_write_amp(codec, route->path[i], false, 0u,
                    channel != 0u,
                    zero_db, caps);
                if (ret) return ret;
            }
        }
    }
    return 0;
}

/** @brief Best-effort exact rollback that retains failed controls for retry.
 * @param codec Probed codec borrowing the live controller.
 * @param route Route with snapshots and zero or more changed control bits.
 * @return 0 when every changed value was restored, otherwise the first error.
 *
 * @par Context
 * Task context after stream stop or an apply failure; caller still owns the transaction gate.
 * @par Ownership and locks/IRQ
 * Continues independent restore attempts after an error, clearing only verified controls. Failed flags/snapshots stay with route for a later retry; no lock spans a verb.
 */
static int hda_route_restore_controls(struct hda_codec *codec,
                                      struct hda_route *route)
{
    int first_error = 0;
    int ret;
    ret = hda_route_gate_pins(codec, route);
    if (ret) return ret;
    for (uint32_t i = 0u; i < route->path_count; ++i) {
        if (route->changed_flags[i] & HDA_ROUTE_CHANGED_INPUT_AMP) {
            ret = hda_route_mute_amp(codec, route, i, true, false);
            if (ret && !first_error) first_error = ret;
            ret = hda_route_mute_amp(codec, route, i, true, true);
            if (ret && !first_error) first_error = ret;
        }
        if (route->changed_flags[i] & HDA_ROUTE_CHANGED_OUTPUT_AMP) {
            ret = hda_route_mute_amp(codec, route, i, false, false);
            if (ret && !first_error) first_error = ret;
            ret = hda_route_mute_amp(codec, route, i, false, true);
            if (ret && !first_error) first_error = ret;
        }
    }

    for (uint32_t i = route->path_count; i > 0u; --i) {
        uint32_t index = i - 1u;
        uint8_t flags = route->changed_flags[index];
        if (flags & HDA_ROUTE_CHANGED_EAPD) {
            ret = hda_codec_set_checked(codec, route->path[index],
                                        HDA_VERB_SET_EAPD, HDA_VERB_GET_EAPD,
                                        route->saved_eapd[index], 0xffu);
            if (!ret) route->changed_flags[index] &=
                         (uint8_t)~HDA_ROUTE_CHANGED_EAPD;
            else if (!first_error) first_error = ret;
        }
        if (flags & HDA_ROUTE_CHANGED_PIN) {
            ret = hda_codec_set_checked(codec, route->path[index],
                                        HDA_VERB_SET_PIN_CONTROL,
                                        HDA_VERB_GET_PIN_CONTROL,
                                        route->saved_pin_control[index], 0xffu);
            if (!ret) route->changed_flags[index] &=
                         (uint8_t)~HDA_ROUTE_CHANGED_PIN;
            else if (!first_error) first_error = ret;
        }
        if (flags & HDA_ROUTE_CHANGED_SELECTOR) {
            ret = hda_codec_set_checked(codec, route->path[index],
                                        HDA_VERB_SET_CONNECTION_SELECT,
                                        HDA_VERB_GET_CONNECTION_SELECT,
                                        route->saved_selector[index], 0xffu);
            if (!ret) route->changed_flags[index] &=
                         (uint8_t)~HDA_ROUTE_CHANGED_SELECTOR;
            else if (!first_error) first_error = ret;
        }
    }

    for (uint32_t i = route->path_count; i > 0u; --i) {
        uint32_t index = i - 1u;
        if (route->changed_flags[index] & HDA_ROUTE_CHANGED_INPUT_AMP) {
            int left_ret = hda_route_restore_amp(codec, route, index, true,
                                                false);
            int right_ret = hda_route_restore_amp(codec, route, index, true,
                                                 true);
            if (left_ret && !first_error) first_error = left_ret;
            if (right_ret && !first_error) first_error = right_ret;
            if (!left_ret && !right_ret)
                route->changed_flags[index] &=
                    (uint8_t)~HDA_ROUTE_CHANGED_INPUT_AMP;
        }
        if (route->changed_flags[index] & HDA_ROUTE_CHANGED_OUTPUT_AMP) {
            int left_ret = hda_route_restore_amp(codec, route, index, false,
                                                false);
            int right_ret = hda_route_restore_amp(codec, route, index, false,
                                                 true);
            if (left_ret && !first_error) first_error = left_ret;
            if (right_ret && !first_error) first_error = right_ret;
            if (!left_ret && !right_ret)
                route->changed_flags[index] &=
                    (uint8_t)~HDA_ROUTE_CHANGED_OUTPUT_AMP;
        }
    }

    for (uint32_t i = route->path_count; i > 0u; --i) {
        uint32_t index = i - 1u;
        if (!(route->changed_flags[index] & HDA_ROUTE_CHANGED_POWER)) continue;
        ret = hda_codec_set_power_state(codec, route->path[index],
                                        route->saved_power[index]);
        if (!ret) route->changed_flags[index] &=
                     (uint8_t)~HDA_ROUTE_CHANGED_POWER;
        else if (!first_error) first_error = ret;
    }
    return first_error;
}

/** @brief Apply one route while the enclosing codec route gate is owned.
 * @param codec Probed metadata whose logical route gate is held.
 * @param route Path resolved for this codec and caller-owned snapshot storage.
 * @param enable True to snapshot/apply; false to retry or complete restoration.
 * @return 0 on success or the first route, transport, or ownership error.
 * Failed rollback retains active state and snapshots for a later retry.
 *
 * @par Context
 * Task context outside global execution ownership and IRQ/service callbacks.
 * @par Ownership and locks/IRQ
 * Caller owns the logical gate; no spinlock spans verbs or readiness waits.
 */
static int hda_apply_route_locked(struct hda_codec *codec,
                                  struct hda_route *route, bool enable)
{
    int ret = hda_route_validate(codec, route);
    if (ret) return ret;
    if (enable) {
        if (route->active || route->snapshots_valid) return -EBUSY;
        if ((route->direction == HDA_ROUTE_PLAYBACK && !codec->has_playback) ||
            (route->direction == HDA_ROUTE_CAPTURE && !codec->has_capture))
            return -ENODEV;
        ret = hda_route_check_control_lease(codec, route);
        if (ret) return ret;
        ret = hda_route_take_snapshot(codec, route);
        if (ret) return ret;
        ret = hda_route_acquire_leases(codec, route);
        if (ret) {
            route->snapshots_valid = 0u;
            return ret;
        }
        ret = hda_route_apply_controls(codec, route);
        if (ret) {
            int apply_error = ret;
            int rollback = hda_route_restore_controls(codec, route);
            if (!rollback) rollback = hda_route_release_leases(codec, route);
            if (rollback) {
                route->active = 1u;
                route->rollback_failed = 1u;
                route->rollback_error = rollback;
                ++codec->active_routes;
                return rollback;
            }
            route->snapshots_valid = 0u;
            route->rollback_failed = 0u;
            route->rollback_error = 0;
            return apply_error;
        }
        route->active = 1u;
        route->rollback_failed = 0u;
        route->rollback_error = 0;
        ++codec->active_routes;
        return 0;
    }
    if (!route->active || !route->snapshots_valid || !codec->active_routes)
        return -EINVAL;
    ret = hda_route_restore_controls(codec, route);
    if (!ret) ret = hda_route_release_leases(codec, route);
    if (ret) {
        route->rollback_failed = 1u;
        route->rollback_error = ret;
        return ret;
    }
    route->active = 0u;
    route->rollback_failed = 0u;
    route->rollback_error = 0;
    route->snapshots_valid = 0u;
    --codec->active_routes;
    return 0;
}

/** @brief Apply one route or retry exact restoration.
 * @param codec Probed codec whose logical route gate serializes operations.
 * @param route Route returned by hda_find_route and owned by the caller.
 * @param enable True to apply; false to restore after PCM has stopped.
 * @return 0 on success or a route, transport, or ownership error. Failed
 * rollback retains the active route and snapshots for a later disable retry.
 *
 * @par Context
 * Task context outside global execution ownership and IRQ/service callbacks.
 * @par Ownership and locks/IRQ
 * One bounded nonspinning logical gate covers the transaction; it is not a
 * spinlock and no lock spans verbs or sleep. Caller serializes direct codec
 * control operations, stops PCM before topology changes, and keeps route and
 * codec objects alive until restoration completes.
 */
int hda_apply_route(struct hda_codec *codec, struct hda_route *route,
                    bool enable)
{
    if (!codec || !route) return -EINVAL;
    int ret = hda_codec_route_gate_enter(codec);
    if (ret) return ret;
    ret = hda_apply_route_locked(codec, route, enable);
    hda_codec_route_gate_leave(codec);
    return ret;
}

/** @brief Validate group paths, channel ranges, and nonsharing constraints.
 * @param codec Probed codec that owns the immutable topology.
 * @param group Caller-owned group and its exact-page route members.
 * @return 0 for a complete feasible group, -EINVAL for stale/malformed data.
 *
 * @par Context
 * Task context while the codec logical route gate is held.
 * @par Ownership and locks/IRQ
 * Reads route/group and codec metadata only; no I/O, allocation, or mutation.
 */
static int hda_route_group_validate(const struct hda_codec *codec,
                                    const struct hda_route_group *group)
{
    if (!codec || !group || group->codec != codec || !group->members ||
        !group->members_phys || !group->members_pages ||
        !group->member_count ||
        group->member_count > HDA_ROUTE_GROUP_MAX_MEMBERS ||
        !group->association || group->association > 15u ||
        (group->association == 15u && group->member_count != 1u) ||
        !group->total_channels || group->total_channels > 16u ||
        !codec->route_groups) return -EINVAL;
    uint32_t next_channel = 0u;
    uint8_t afg_nid = 0u;
    uint8_t used_nodes[HDA_CODEC_MAX_NIDS];
    hda_codec_zero(used_nodes, sizeof(used_nodes));
    for (uint32_t member = 0u; member < group->member_count; ++member) {
        const struct hda_route *route = &group->members[member];
        if (hda_route_validate(codec, route) ||
            hda_route_group_amp_indices(codec, route) ||
            route->direction != HDA_ROUTE_PLAYBACK ||
            route->association != group->association ||
            route->channel_start != next_channel || !route->channel_count ||
            next_channel + route->channel_count > 16u)
            return -EINVAL;
        uint8_t route_afg = codec->nodes[route->pin_nid].afg_nid;
        if (member && route_afg != afg_nid) return -EINVAL;
        afg_nid = route_afg;
        if (member && group->members[member - 1u].sequence >= route->sequence)
            return -EINVAL;
        uint8_t actual_channels = hda_route_path_capacity(codec, route);
        if (route->channel_count != actual_channels) return -EINVAL;
        for (uint32_t path = 0u; path < route->path_count; ++path) {
            uint8_t nid = route->path[path];
            if (used_nodes[nid]) return -EINVAL;
            used_nodes[nid] = 1u;
        }
        next_channel += route->channel_count;
    }
    return next_channel == group->total_channels ? 0 : -EINVAL;
}

/** @brief Apply or restore every member under one nonspinning route gate.
 * @param codec Probed metadata owning the group and route transaction gate.
 * @param group Live group returned by hda_find_route_group.
 * @param enable True to activate all members; false to restore/retry them.
 * @return 0 after all members transition or the first route/rollback error.
 * Partial state remains active with snapshots and leases available for retry.
 *
 * @par Context
 * Task context with PCM stopped during topology changes; not IRQ/service or
 * global execution ownership context.
 * @par Ownership and locks/IRQ
 * One bounded nonspinning codec gate covers preflight, all verbs/waits, and
 * rollback; no spinlock spans a verb or sleep. Group and codec lifetimes remain
 * caller-owned until every route is restored and the group is destroyed.
 */
int hda_apply_route_group(struct hda_codec *codec,
                          struct hda_route_group *group, bool enable)
{
    if (!codec || !group) return -EINVAL;
    int ret = hda_codec_route_gate_enter(codec);
    if (ret) return ret;
    ret = hda_route_group_validate(codec, group);
    if (ret) goto done;

    if (enable) {
        if (group->active) {
            ret = -EBUSY;
            goto done;
        }
        /* Reject all conflicts before taking snapshots or issuing any write. */
        for (uint32_t member = 0u; member < group->member_count; ++member) {
            struct hda_route *route = &group->members[member];
            if (route->active || route->snapshots_valid) {
                ret = -EBUSY;
                goto done;
            }
            ret = hda_route_check_control_lease(codec, route);
            if (ret) goto done;
        }
        uint32_t completed = 0u;
        for (; completed < group->member_count; ++completed) {
            ret = hda_apply_route_locked(codec, &group->members[completed],
                                         true);
            if (ret) break;
        }
        if (ret) {
            int original_error = ret;
            int rollback_error = 0;
            uint32_t rollback_end = completed +
                (group->members[completed].active ? 1u : 0u);
            while (rollback_end) {
                struct hda_route *route = &group->members[--rollback_end];
                if (!route->active) continue;
                int rollback = hda_apply_route_locked(codec, route, false);
                if (rollback && !rollback_error) rollback_error = rollback;
            }
            for (uint32_t member = 0u; member < group->member_count; ++member)
                if (group->members[member].active) group->active = 1u;
            group->rollback_failed = group->active;
            group->rollback_error = rollback_error;
            ret = rollback_error ? rollback_error : original_error;
            goto done;
        }
        group->active = 1u;
        group->rollback_failed = 0u;
        group->rollback_error = 0;
        ret = 0;
    } else {
        if (!group->active) {
            ret = -EINVAL;
            goto done;
        }
        int first_error = 0;
        for (uint32_t member = group->member_count; member > 0u; --member) {
            struct hda_route *route = &group->members[member - 1u];
            if (!route->active) continue;
            int stop = hda_apply_route_locked(codec, route, false);
            if (stop && !first_error) first_error = stop;
        }
        group->active = 0u;
        for (uint32_t member = 0u; member < group->member_count; ++member)
            if (group->members[member].active) group->active = 1u;
        group->rollback_failed = group->active;
        group->rollback_error = group->active ? first_error : 0;
        ret = first_error;
    }

done:
    hda_codec_route_gate_leave(codec);
    return ret;
}

/** @brief Append bounded text to a caller-provided diagnostic line.
 * @param out Output line buffer.
 * @param used Current output cursor, updated on success/truncation.
 * @param capacity Total bytes in the buffer including terminator space.
 * @param text NUL-terminated text to append.
 * @return None; appends as much as fits while retaining a terminator slot.
 *
 * @par Context
 * Task-context codec diagnostics.
 * @par Ownership and locks/IRQ
 * Caller owns the stack buffer; no allocation, lock, or device access.
 */
static void hda_codec_append_text(char *out, uint32_t *used, uint32_t capacity,
                                  const char *text)
{
    if (!out || !used || !text || !capacity) return;
    while (*text && *used + 1u < capacity) out[(*used)++] = *text++;
    out[*used] = '\0';
}

/** @brief Append a fixed-width lowercase hexadecimal value to a diagnostic line.
 * @param out Output line buffer.
 * @param used Current output cursor, updated after emitted digits.
 * @param capacity Total bytes in the line including terminator space.
 * @param value Value whose least significant @p digits are emitted.
 * @param digits Number of hexadecimal digits, from 1 through 8.
 * @return None; emits bounded numeric text without revealing pointers.
 *
 * @par Context
 * Task-context codec diagnostics.
 * @par Ownership and locks/IRQ
 * Caller owns the output buffer; no allocation, lock, or hardware access.
 */
static void hda_codec_append_hex(char *out, uint32_t *used, uint32_t capacity,
                                 uint32_t value, uint8_t digits)
{
    static const char hex[] = "0123456789abcdef";
    if (!out || !used || !capacity || digits > 8u) return;
    while (digits && *used + 1u < capacity) {
        uint8_t shift = (uint8_t)((digits - 1u) * 4u);
        out[(*used)++] = hex[(value >> shift) & 0x0fu];
        --digits;
    }
    out[*used] = '\0';
}

/** @brief Emit codec identity, NID capabilities/defaults, and expanded graph edges.
 * @param codec Successfully probed immutable codec metadata.
 * @return None; no output is emitted for a missing cache or console callback.
 *
 * @par Context
 * Task diagnostics while probe metadata is alive; this does not query hardware.
 * @par Ownership and locks/IRQ
 * Reads CPU metadata only and borrows console_write. Output contains numeric codec/NID values and never a kernel address.
 */
void hda_codec_dump(const struct hda_codec *codec)
{
    if (!codec || !codec->probed || !codec->api || !codec->api->console_write)
        return;
    char line[192];
    uint32_t used = 0u;
    line[0] = '\0';
    hda_codec_append_text(line, &used, sizeof(line), "[hda] codec cad=0x");
    hda_codec_append_hex(line, &used, sizeof(line), codec->cad, 2u);
    hda_codec_append_text(line, &used, sizeof(line), " vendor=0x");
    hda_codec_append_hex(line, &used, sizeof(line), codec->vendor_id, 8u);
    hda_codec_append_text(line, &used, sizeof(line), " revision=0x");
    hda_codec_append_hex(line, &used, sizeof(line), codec->revision_id, 8u);
    hda_codec_append_text(line, &used, sizeof(line),
                          " implementation_id(first_afg)=0x");
    hda_codec_append_hex(line, &used, sizeof(line), codec->subsystem_id, 8u);
    hda_codec_append_text(line, &used, sizeof(line), "\n");
    codec->api->console_write(line);

    for (uint32_t nid = 1u; nid < HDA_CODEC_MAX_NIDS; ++nid) {
        const struct hda_codec_node *node = &codec->nodes[nid];
        if (!node->present) continue;
        used = 0u;
        line[0] = '\0';
        hda_codec_append_text(line, &used, sizeof(line), "[hda] nid=0x");
        hda_codec_append_hex(line, &used, sizeof(line), nid, 2u);
        hda_codec_append_text(line, &used, sizeof(line), " type=0x");
        hda_codec_append_hex(line, &used, sizeof(line), node->type, 2u);
        hda_codec_append_text(line, &used, sizeof(line), " caps=0x");
        hda_codec_append_hex(line, &used, sizeof(line), node->widget_caps, 8u);
        hda_codec_append_text(line, &used, sizeof(line), " pin_caps=0x");
        hda_codec_append_hex(line, &used, sizeof(line), node->pin_caps, 8u);
        hda_codec_append_text(line, &used, sizeof(line), " default=0x");
        hda_codec_append_hex(line, &used, sizeof(line), node->pin_default, 8u);
        hda_codec_append_text(line, &used, sizeof(line), " rates=0x");
        hda_codec_append_hex(line, &used, sizeof(line), node->pcm_rates, 8u);
        hda_codec_append_text(line, &used, sizeof(line), " formats=0x");
        hda_codec_append_hex(line, &used, sizeof(line), node->stream_formats, 8u);
        if (node->type == HDA_WIDGET_FUNCTION_GROUP) {
            hda_codec_append_text(line, &used, sizeof(line),
                                  " implementation_id=0x");
            hda_codec_append_hex(line, &used, sizeof(line),
                                 node->implementation_id, 8u);
        }
        hda_codec_append_text(line, &used, sizeof(line), " connections=0x");
        hda_codec_append_hex(line, &used, sizeof(line), node->connection_count, 3u);
        hda_codec_append_text(line, &used, sizeof(line), "\n");
        codec->api->console_write(line);

        const uint8_t *connections = NULL;
        if (hda_codec_connection_slice(codec, node, &connections)) continue;
        for (uint32_t i = 0u; i < node->connection_count; ++i) {
            used = 0u;
            line[0] = '\0';
            hda_codec_append_text(line, &used, sizeof(line),
                                  "[hda] connection owner=0x");
            hda_codec_append_hex(line, &used, sizeof(line), nid, 2u);
            hda_codec_append_text(line, &used, sizeof(line), " index=0x");
            hda_codec_append_hex(line, &used, sizeof(line), i, 2u);
            hda_codec_append_text(line, &used, sizeof(line), " nid=0x");
            hda_codec_append_hex(line, &used, sizeof(line), connections[i], 2u);
            hda_codec_append_text(line, &used, sizeof(line), "\n");
            codec->api->console_write(line);
        }
    }
}

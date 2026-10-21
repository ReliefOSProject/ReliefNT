#include "codec_internal.h"

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
                                   uint32_t *out_value);

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
                                     uint8_t *out_count);

/** @brief Reclaim the exact CPU-only metadata runs held by a partial or full cache.
 * @param codec Cache whose physical bases and page counts are still intact.
 * @return None; each owned run is freed once with the original allocator tuple.
 *
 * @par Context
 * Task-context probe unwind or codec teardown after route users have stopped.
 * @par Ownership and locks/IRQ
 * Frees only codec-owned alloc_pages runs; never touches controller DMA/MMIO and never waits with a lock held.
 */
static void hda_codec_free_metadata(struct hda_codec *codec);

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
                                      uint32_t *used);

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
                                  uint32_t *connection_bytes);


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
void hda_codec_zero(void *storage, uint64_t byte_count)
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
void *hda_codec_map_metadata_run(uint64_t phys, uint32_t pages)
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

/** @brief Emit codec identity, NID capabilities/defaults, and expanded graph edges.
 * @param codec Successfully probed immutable codec metadata.
 * @return None; no output is emitted for a missing cache or console callback.
 *
 * @par Context
 * Task diagnostics while probe metadata is alive; this does not query hardware.
 * @par Ownership and locks/IRQ
 * Reads CPU metadata only and borrows console_log. Output contains numeric codec/NID values and never a kernel address.
 */
void hda_codec_dump(const struct hda_codec *codec)
{
    if (!codec || !codec->probed || !codec->api ||
        codec->api->struct_size < RELIEFOS_DRIVER_LOG_API_SIZE || !codec->api->console_log)
        return;
    const struct reliefos_driver_kernel_api *api = codec->api;
    api->console_log(CONSOLE_LOG_DEBUG,
        "[hda] codec cad=0x%02x vendor=0x%08x revision=0x%08x implementation_id(first_afg)=0x%08x\n",
        (unsigned)codec->cad, codec->vendor_id, codec->revision_id, codec->subsystem_id);

    for (uint32_t nid = 1u; nid < HDA_CODEC_MAX_NIDS; ++nid) {
        const struct hda_codec_node *node = &codec->nodes[nid];
        if (!node->present) continue;
        api->console_log(CONSOLE_LOG_DEBUG,
            "[hda] nid=0x%02x type=0x%02x caps=0x%08x pin_caps=0x%08x default=0x%08x "
            "rates=0x%08x formats=0x%08x connections=0x%03x\n",
            nid, (unsigned)node->type, node->widget_caps, node->pin_caps,
            node->pin_default, node->pcm_rates, node->stream_formats,
            (unsigned)node->connection_count);
        if (node->type == HDA_WIDGET_FUNCTION_GROUP)
            api->console_log(CONSOLE_LOG_DEBUG,
                "[hda] nid=0x%02x implementation_id=0x%08x\n", nid, node->implementation_id);

        const uint8_t *connections = NULL;
        if (hda_codec_connection_slice(codec, node, &connections)) continue;
        for (uint32_t index = 0u; index < node->connection_count; ++index)
            api->console_log(CONSOLE_LOG_DEBUG,
                "[hda] connection owner=0x%02x index=0x%02x nid=0x%02x\n",
                nid, index, (unsigned)connections[index]);
    }
}

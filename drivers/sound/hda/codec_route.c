#include "codec_internal.h"

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
                                         enum hda_route_direction direction);

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
                   const uint8_t *used_converters, uint8_t skip_converter);

/** @brief Decode the 1-based channel count encoded by WidgetCaps.
 * @param caps Raw widget capabilities.
 * @return Number of supported channels in the range 1..16.
 *
 * @par Context
 * Task-context topology planning over immutable cached capabilities.
 * @par Ownership and locks/IRQ
 * Pure bit extraction; no mutation, allocation, lock, or I/O.
 */
static uint8_t hda_codec_channel_count(uint32_t caps);

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
                                       const struct hda_route *route);

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
                                       const struct hda_route *route);

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
                                 uint32_t *budget);

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
                                         struct hda_route *route);


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
int hda_codec_connection_slice(const struct hda_codec *codec,
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
int hda_route_validate(const struct hda_codec *codec,
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
int hda_route_check_control_lease(const struct hda_codec *codec,
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
int hda_route_group_validate(const struct hda_codec *codec,
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

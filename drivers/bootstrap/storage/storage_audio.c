/* Registry-backed Linux sound namespace; no disk or MMIO access. */
#include <reliefnt/audio.h>
#include <reliefnt/storage.h>
#include <string.h>

/* storage_audio.c is also compiled directly by the focused devfs fixture,
 * where storage_state.c (the normal owner of storage_copy_text) is not
 * included.  Keep this small bounded copier local so both build paths share
 * the same production parser without relying on include order. */
static void storage_audio_copy_text(char *out, uint32_t cap, const char *text)
{
    if (!out || !cap) return;
    uint32_t i = 0;
    if (text) while (i + 1u < cap && text[i]) {
        out[i] = text[i];
        ++i;
    }
    out[i] = 0;
}

/** @brief Resolve a current devfs card ordinal to its generation token.
 * @param ordinal Compact card number. @param token Receives the identity.
 * @return Zero or a registry error; task context, no ownership transfer.
 */
static int storage_audio_card_token(uint32_t ordinal, uint32_t *token)
{
    return audio_card_snapshot(ordinal, token, NULL);
}

/** @brief Parse one bounded decimal card number without integer overflow.
 * @param cursor In/out name position. @param value Receives 0..31.
 * @return Zero or -EINVAL; no mutation of storage or registry state.
 */
static int storage_audio_decimal(const char **cursor, uint32_t *value)
{
    const char *p = *cursor;
    uint32_t result = 0;
    if (!p || *p < '0' || *p > '9') return -22;
    do {
        uint32_t digit = (uint32_t)(*p++ - '0');
        if (result > (31u - digit) / 10u) return -22;
        result = result * 10u + digit;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *value = result;
    return 0;
}

/** @brief Look up a registered Linux control node.
 * @param name Basename. @param out Optional borrowed output storage.
 * @return Zero or -ENOENT; task context, only a registry snapshot is acquired.
 */
static int storage_audio_control_node(const char *name, struct storage_node *out)
{
    const char *p = name;
    uint32_t ordinal, token;
    if (__builtin_strncmp(p, "controlC", 8) != 0) return -2;
    p += 8;
    if (storage_audio_decimal(&p, &ordinal) < 0 || *p ||
        storage_audio_card_token(ordinal, &token) < 0) return -2;
    if (out) *out = (struct storage_node){
        .type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE | STORAGE_NODE_FLAG_AUDIO_CONTROL,
        .first_cluster = STORAGE_DEV_KIND_AUDIO_CONTROL, .volume_id = token,
    };
    return 0;
}

/** @brief Look up a supported PCM device and direction without opening it.
 * @param name Linux PCM basename. @param out Optional borrowed node output.
 * @return Zero or -ENOENT; caps callbacks run outside registry locks.
 */
static int storage_audio_pcm_node(const char *name, struct storage_node *out)
{
    uint32_t ordinal, device, token;
    enum audio_direction direction;
    struct audio_caps caps;
    if (audio_device_parse_name(name, &ordinal, &device, &direction) < 0 ||
        storage_audio_card_token(ordinal, &token) < 0 ||
        audio_card_pcm_caps(token, device, direction, &caps) < 0) return -2;
    if (out) *out = (struct storage_node){
        .type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE | STORAGE_NODE_FLAG_AUDIO_PCM,
        .first_cluster = STORAGE_DEV_KIND_AUDIO,
        .volume_id = AUDIO_DEVICE_VOLUME_ID(token, device, direction),
    };
    return 0;
}

/** @brief Look up the global ALSA timer character node. */
static int storage_audio_timer_node(const char *name, struct storage_node *out)
{
    if (!name || __builtin_strcmp(name, "timer") != 0) return -2;
    if (out) *out = (struct storage_node){
        .type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE | STORAGE_NODE_FLAG_AUDIO_TIMER,
        .first_cluster = STORAGE_DEV_KIND_AUDIO_TIMER,
    };
    return 0;
}

/** @brief Enumerate only registered control and supported PCM nodes.
 * @param cursor In/out candidate index: 17 positions per card (control + 8*2).
 * @param entry Receives one Linux basename and character-device type.
 * @return One entry, zero at EOF, or -EINVAL; bounded task-context snapshots.
 */
static int storage_audio_snd_readdir(uint64_t *cursor, struct reliefos_dir_entry *entry)
{
    if (!cursor || !entry) return -22;
    if (*cursor == 0) {
        ++*cursor;
        entry->type = RELIEFOS_FS_TYPE_DEVICE;
        storage_audio_copy_text(entry->name, sizeof(entry->name), "timer");
        return 1;
    }
    while (*cursor <= 32u * 17u) {
        uint64_t index = *cursor - 1u;
        uint32_t ordinal = (uint32_t)(index / 17u);
        uint32_t item = (uint32_t)(index % 17u);
        uint32_t token;
        if (storage_audio_card_token(ordinal, &token) < 0) return 0;
        ++*cursor;
        if (item) {
            struct audio_caps caps;
            if (audio_card_pcm_caps(token, (item - 1u) / 2u,
                    (item - 1u) % 2u ? AUDIO_CAPTURE : AUDIO_PLAYBACK, &caps) < 0)
                continue;
        }
        char *name = entry->name;
        const char *prefix = item ? "pcmC" : "controlC";
        uint32_t pos = 0;
        while (*prefix) name[pos++] = *prefix++;
        if (ordinal >= 10u) name[pos++] = (char)('0' + ordinal / 10u);
        name[pos++] = (char)('0' + ordinal % 10u);
        if (item) {
            name[pos++] = 'D';
            name[pos++] = (char)('0' + (item - 1u) / 2u);
            name[pos++] = (item - 1u) % 2u ? 'c' : 'p';
        }
        name[pos] = 0;
        entry->type = RELIEFOS_FS_TYPE_DEVICE;
        return 1;
    }
    return 0;
}

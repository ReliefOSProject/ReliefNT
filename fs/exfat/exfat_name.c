#include "internal.h"
#include "exfat_upcase_data.h"

static uint16_t exfat_upcase(uint16_t value);

static uint16_t storage_exfat_upcase_cache[STORAGE_MAX_VOLUMES][65536];
static uint8_t storage_exfat_upcase_data[65536u * sizeof(uint16_t)]
    __attribute__((aligned(4096)));
uint16_t exfat_get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void exfat_put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

uint64_t exfat_get_u64(const uint8_t *p)
{
    return (uint64_t)storage_get_u32(p) | ((uint64_t)storage_get_u32(p + 4u) << 32);
}

void exfat_put_u64(uint8_t *p, uint64_t value)
{
    storage_put_u32(p, (uint32_t)value);
    storage_put_u32(p + 4u, (uint32_t)(value >> 32));
}

int exfat_load_upcase_table(void)
{
    uint16_t *table;
    uint64_t offset = 0;
    uint32_t codepoint = 0;
    uint32_t checksum = 0;
    uint32_t raw_length;
    if (g_storage.volume_id >= STORAGE_MAX_VOLUMES || !g_storage.exfat_upcase_length ||
        g_storage.exfat_upcase_length > sizeof(storage_exfat_upcase_data) ||
        (g_storage.exfat_upcase_length & 1u) != 0u ||
        !exfat_cluster_valid(g_storage.exfat_upcase_cluster)) {
        console_error("[reliefnt] exfat upcase parameters invalid volume=%u cluster=%u length=%llu count=%u\n",
                       g_storage.volume_id, g_storage.exfat_upcase_cluster,
                       (unsigned long long)g_storage.exfat_upcase_length,
                       g_storage.exfat_cluster_count);
        return -22;
    }
    table = storage_exfat_upcase_cache[g_storage.volume_id];
    raw_length = (uint32_t)g_storage.exfat_upcase_length;
    {
        int ret = exfat_read_stream(g_storage.exfat_upcase_cluster, g_storage.exfat_upcase_nofat,
                                    0, storage_exfat_upcase_data, raw_length);
        if (ret < 0) {
        console_error("[reliefnt] exfat upcase read failed offset=0 length=%u first=%u nofat=%u\n",
                       raw_length, g_storage.exfat_upcase_cluster,
                       g_storage.exfat_upcase_nofat);
            return ret;
        }
    }
    for (uint32_t i = 0; i < raw_length; ++i) {
        checksum = (checksum << 31) | (checksum >> 1);
        checksum += storage_exfat_upcase_data[i];
    }
    if (checksum != g_storage.exfat_upcase_checksum) {
        console_warn("[reliefnt] exfat upcase checksum mismatch calculated=%u stored=%u first=%u,%u,%u,%u nofat=%u\n",
                       checksum, g_storage.exfat_upcase_checksum,
                       storage_exfat_upcase_data[0], storage_exfat_upcase_data[1],
                       storage_exfat_upcase_data[2], storage_exfat_upcase_data[3],
                       g_storage.exfat_upcase_nofat);
        return -5;
    }
    offset = 0;
    while (offset + 2u <= g_storage.exfat_upcase_length && codepoint < 65536u) {
        uint16_t value;
        value = exfat_get_u16(storage_exfat_upcase_data + offset);
        offset += 2u;
        /* 0xFFFF is the compressed-run marker only when another UTF-16
         * count follows it.  The standard table legitimately ends with a
         * literal U+FFFF mapping, so accept a terminal marker as one value. */
        if (value != 0xffffu || offset >= g_storage.exfat_upcase_length) {
            table[codepoint++] = value;
            continue;
        }
        if (offset + 2u > g_storage.exfat_upcase_length) {
            return -5;
        }
        value = exfat_get_u16(storage_exfat_upcase_data + offset);
        offset += 2u;
        if (value == 0u || value > 65536u - codepoint) {
            console_error("[reliefnt] exfat upcase decode run invalid value=%u codepoint=%u\n",
                           value, codepoint);
            return -5;
        }
        while (value--) table[codepoint] = (uint16_t)codepoint, ++codepoint;
    }
    if (codepoint != 65536u) {
        console_error("[reliefnt] exfat upcase decode incomplete codepoint=%u offset=%llu\n",
                       codepoint, (unsigned long long)offset);
        return -5;
    }
    storage_exfat_upcase_ready[g_storage.volume_id] = 1u;
    return 0;
}

static uint16_t exfat_upcase(uint16_t value)
{
    if (g_storage.volume_id < STORAGE_MAX_VOLUMES &&
        storage_exfat_upcase_ready[g_storage.volume_id]) {
        return storage_exfat_upcase_cache[g_storage.volume_id][value];
    }
    return value >= 'a' && value <= 'z' ? (uint16_t)(value - 'a' + 'A') : value;
}

uint16_t exfat_name_hash(const uint16_t *name, uint32_t length)
{
    uint16_t hash = 0;
    for (uint32_t i = 0; i < length; ++i) {
        uint16_t value = exfat_upcase(name[i]);
        uint8_t lo = (uint8_t)value;
        uint8_t hi = (uint8_t)(value >> 8);
        hash = (uint16_t)((hash << 15) | (hash >> 1));
        hash = (uint16_t)(hash + lo);
        hash = (uint16_t)((hash << 15) | (hash >> 1));
        hash = (uint16_t)(hash + hi);
    }
    return hash;
}

int exfat_utf8_name(const char *name, uint16_t out[255], uint32_t *out_length)
{
    uint32_t length;
    if (!name || !name[0] || !out || !out_length) return -22;
    length = (uint32_t)storage_strlen(name);
    if (length >= RELIEFOS_FS_NAME_LEN || storage_text_eq(name, ".") ||
        storage_text_eq(name, "..")) return -22;
    for (uint32_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 0x20u || name[i] == '"' || name[i] == '*' || name[i] == '/' ||
            name[i] == ':' || name[i] == '<' || name[i] == '>' || name[i] == '?' ||
            name[i] == '\\' || name[i] == '|') return -22;
    }
    if (text_utf8_to_utf16le(name, length, out, 255u, out_length) < 0 ||
        *out_length == 0 || *out_length > 255u) return -22;
    return 0;
}

int exfat_name_equals(const uint16_t *name, uint32_t length, const char *other)
{
    uint16_t other16[255];
    uint32_t other_length;
    if (exfat_utf8_name(other, other16, &other_length) < 0 || other_length != length) return 0;
    for (uint32_t i = 0; i < length; ++i) {
        if (exfat_upcase(name[i]) != exfat_upcase(other16[i])) return 0;
    }
    return 1;
}

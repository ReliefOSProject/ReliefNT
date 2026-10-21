#include "internal.h"

static int fat32_is_short_compatible_char(char ch);

static char fat32_upper_char(char ch);

static int fat32_short_name_eq(const uint8_t lhs[11], const uint8_t rhs[11]);

static int fat32_is_lfn_char_valid(char ch);

static void fat32_name_split(const char *name,
                             const char **base_start, uint32_t *base_len,
                             const char **ext_start, uint32_t *ext_len);

static void fat32_collect_short_fragment(const char *src, uint32_t len,
                                         char *dst, uint32_t cap);

static int fat32_short_name_exists_in_dir(uint32_t dir_cluster, const uint8_t short_name[11]);


static int fat32_is_short_compatible_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '-';
}

static char fat32_upper_char(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return (char)(ch - 'a' + 'A');
    }
    return ch;
}

int fat32_make_short_name(const char *name, uint8_t out[11])
{
    uint32_t dot = 0xffffffffu;
    uint32_t len = 0;
    if (!name || !name[0] || !out) {
        return -22;
    }
    while (name[len]) {
        if (name[len] == '.') {
            if (dot != 0xffffffffu) {
                return -22;
            }
            dot = len;
        } else if (!fat32_is_short_compatible_char(name[len])) {
            return -22;
        }
        ++len;
    }
    if (dot == 0 || len == 0) {
        return -22;
    }
    if (dot == 0xffffffffu) {
        dot = len;
    }
    if (dot > 8 || (len - dot - (dot < len ? 1u : 0u)) > 3) {
        return -22;
    }
    for (uint32_t i = 0; i < 11; ++i) {
        out[i] = ' ';
    }
    for (uint32_t i = 0; i < dot; ++i) {
        out[i] = (uint8_t)fat32_upper_char(name[i]);
    }
    if (dot < len) {
        for (uint32_t i = dot + 1, j = 8; i < len; ++i, ++j) {
            out[j] = (uint8_t)fat32_upper_char(name[i]);
        }
    }
    return 0;
}

static int fat32_short_name_eq(const uint8_t lhs[11], const uint8_t rhs[11])
{
    return storage_memcmp(lhs, rhs, 11) == 0;
}

int fat32_name_match_short(const struct fat32_dirent *de, const char *name)
{
    char short_name[RELIEFOS_FS_NAME_LEN];
    uint32_t pos = 0;
    for (uint32_t i = 0; i < 8 && de->name[i] != ' '; ++i) {
        char ch = (char)de->name[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        if (pos + 1 < sizeof(short_name)) {
            short_name[pos++] = ch;
        }
    }
    if (de->name[8] != ' ') {
        if (pos + 1 < sizeof(short_name)) {
            short_name[pos++] = '.';
        }
        for (uint32_t i = 8; i < 11 && de->name[i] != ' '; ++i) {
            char ch = (char)de->name[i];
            if (ch >= 'A' && ch <= 'Z') {
                ch = (char)(ch - 'A' + 'a');
            }
            if (pos + 1 < sizeof(short_name)) {
                short_name[pos++] = ch;
            }
        }
    }
    short_name[pos] = 0;
    return storage_text_eq_ci(short_name, name);
}

void fat32_lfn_extract_utf16(const struct fat32_lfn *lfn, uint16_t *dst,
                                    uint32_t *len, uint32_t cap)
{
    const uint16_t *parts[3] = {lfn->name1, lfn->name2, lfn->name3};
    const uint32_t counts[3] = {5, 6, 2};
    for (uint32_t p = 0; p < 3; ++p) {
        for (uint32_t i = 0; i < counts[p]; ++i) {
            uint16_t ch = parts[p][i];
            if (ch == 0x0000 || ch == 0xffff) {
                return;
            }
            if (*len < cap) {
                dst[(*len)++] = ch;
            }
        }
    }
}

void fat32_build_lfn_name(const uint16_t lfn_parts[20][13], uint32_t lfn_count,
                                 char *dst, uint32_t cap)
{
    uint16_t utf16[260];
    uint32_t len = 0;
    uint32_t utf8_len = 0;
    if (!dst || cap == 0) {
        return;
    }
    dst[0] = 0;
    for (uint32_t i = 0; i < lfn_count; ++i) {
        for (uint32_t j = 0; j < 13 && len < sizeof(utf16) / sizeof(utf16[0]); ++j) {
            uint16_t ch = lfn_parts[i][j];
            if (ch == 0) {
                break;
            }
            utf16[len++] = ch;
        }
    }
    if (text_utf16le_to_utf8(utf16, len, dst, cap, &utf8_len) < 0) {
        dst[0] = 0;
        return;
    }
    if (utf8_len >= cap) {
        dst[cap - 1] = 0;
    }
}

static int fat32_is_lfn_char_valid(char ch)
{
    unsigned char uch = (unsigned char)ch;
    if (uch < 32) {
        return 0;
    }
    switch (ch) {
    case '"':
    case '*':
    case '/':
    case ':':
    case '<':
    case '>':
    case '?':
    case '\\':
    case '|':
        return 0;
    default:
        return 1;
    }
}

int fat32_validate_name(const char *name)
{
    uint32_t len = (uint32_t)storage_strlen(name);
    uint16_t utf16[260];
    uint32_t utf16_len = 0;
    if (!name || !name[0] || len >= RELIEFOS_FS_NAME_LEN) {
        return -22;
    }
    if (storage_text_eq(name, ".") || storage_text_eq(name, "..")) {
        return -22;
    }
    if (name[len - 1] == ' ' || name[len - 1] == '.') {
        return -22;
    }
    for (uint32_t i = 0; i < len; ++i) {
        if (!fat32_is_lfn_char_valid(name[i])) {
            return -22;
        }
    }
    if (text_utf8_to_utf16le(name, len, utf16,
                             sizeof(utf16) / sizeof(utf16[0]), &utf16_len) < 0 ||
        utf16_len == 0 || utf16_len > 255u) {
        return -22;
    }
    return 0;
}

static void fat32_name_split(const char *name,
                             const char **base_start, uint32_t *base_len,
                             const char **ext_start, uint32_t *ext_len)
{
    uint32_t len = (uint32_t)storage_strlen(name);
    uint32_t dot = 0xffffffffu;
    for (uint32_t i = 0; i < len; ++i) {
        if (name[i] == '.') {
            dot = i;
        }
    }
    *base_start = name;
    *base_len = len;
    *ext_start = 0;
    *ext_len = 0;
    if (dot != 0xffffffffu && dot != 0 && dot + 1u < len) {
        *base_len = dot;
        *ext_start = name + dot + 1u;
        *ext_len = len - dot - 1u;
    }
}

static void fat32_collect_short_fragment(const char *src, uint32_t len,
                                         char *dst, uint32_t cap)
{
    uint32_t out = 0;
    if (!dst || cap == 0) {
        return;
    }
    for (uint32_t i = 0; i < len && out + 1 < cap; ++i) {
        char ch = src[i];
        if (fat32_is_short_compatible_char(ch)) {
            dst[out++] = fat32_upper_char(ch);
        }
    }
    dst[out] = 0;
}

static int fat32_short_name_exists_in_dir(uint32_t dir_cluster, const uint8_t short_name[11])
{
    uint32_t cluster = dir_cluster;
    for (;;) {
        int ret = fat32_read_cluster(cluster, storage_cluster_buf);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        for (uint32_t off = 0; off < g_storage.cluster_bytes; off += sizeof(struct fat32_dirent)) {
            struct fat32_dirent *de = (struct fat32_dirent *)(void *)(storage_cluster_buf + off);
            if (de->name[0] == 0x00) {
                return 0;
            }
            if (de->name[0] == 0xe5 || de->attr == FAT32_ATTR_LFN) {
                continue;
            }
            if (fat32_short_name_eq(de->name, short_name)) {
                return 1;
            }
        }
        {
            uint32_t next = 0;
            ret = fat32_read_fat_entry(cluster, &next);
            if (ret < 0) {
                return storage_read_failure(ret);
            }
            if (next >= FAT32_EOC) {
                return 0;
            }
            cluster = next;
        }
    }
}

int fat32_make_short_alias(uint32_t dir_cluster, const char *name, uint8_t out[11])
{
    const char *base_start = 0;
    const char *ext_start = 0;
    uint32_t base_len = 0;
    uint32_t ext_len = 0;
    char base_part[RELIEFOS_FS_NAME_LEN];
    char ext_part[4];
    fat32_name_split(name, &base_start, &base_len, &ext_start, &ext_len);
    fat32_collect_short_fragment(base_start, base_len, base_part, sizeof(base_part));
    fat32_collect_short_fragment(ext_start, ext_len, ext_part, sizeof(ext_part));
    for (uint32_t ord = 1; ord <= 999999u; ++ord) {
        uint32_t suffix_len = 0;
        uint32_t value = ord;
        char digits[6];
        uint32_t digits_len = 0;
        char tmp[8];
        uint32_t base_limit;
        uint32_t base_copy = 0;
        int exists;

        while (value > 0 && digits_len < sizeof(digits)) {
            digits[digits_len++] = (char)('0' + (value % 10u));
            value /= 10u;
        }
        tmp[suffix_len++] = '~';
        while (digits_len) {
            tmp[suffix_len++] = digits[--digits_len];
        }
        if (suffix_len >= 8) {
            continue;
        }
        base_limit = 8u - suffix_len;
        for (uint32_t i = 0; i < 11; ++i) {
            out[i] = ' ';
        }
        if (!base_part[0]) {
            storage_copy_text(base_part, sizeof(base_part), "FILE");
        }
        while (base_part[base_copy] && base_copy < base_limit) {
            out[base_copy] = (uint8_t)base_part[base_copy];
            ++base_copy;
        }
        for (uint32_t i = 0; i < suffix_len; ++i) {
            out[base_copy + i] = (uint8_t)tmp[i];
        }
        for (uint32_t i = 0; ext_part[i] && i < 3; ++i) {
            out[8 + i] = (uint8_t)ext_part[i];
        }
        exists = fat32_short_name_exists_in_dir(dir_cluster, out);
        if (exists < 0) {
            return exists;
        }
        if (!exists) {
            return 0;
        }
    }
    return -28;
}

uint8_t fat32_short_name_checksum(const uint8_t short_name[11])
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11; ++i) {
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

uint32_t fat32_utf16_name(const char *name, uint16_t *utf16, uint32_t cap)
{
    uint32_t utf16_len = 0;
    if (text_utf8_to_utf16le(name, (uint32_t)storage_strlen(name), utf16, cap,
                             &utf16_len) < 0) {
        return 0;
    }
    return utf16_len;
}

uint32_t fat32_lfn_entry_count(const char *name)
{
    uint16_t utf16[260];
    uint32_t len = fat32_utf16_name(name, utf16, sizeof(utf16) / sizeof(utf16[0]));
    return (len + 12u) / 13u;
}

void fat32_fill_lfn_entry(struct fat32_lfn *lfn,
                                 const uint16_t *name, uint32_t name_len,
                                 uint32_t part_index, uint32_t part_count,
                                 uint8_t checksum)
{
    uint16_t *slots[3] = {lfn->name1, lfn->name2, lfn->name3};
    const uint32_t slot_counts[3] = {5, 6, 2};
    uint32_t cursor = part_index * 13u;
    int terminated = 0;

    storage_memzero(lfn, sizeof(*lfn));
    lfn->order = (uint8_t)(part_index + 1u);
    if (part_index + 1u == part_count) {
        lfn->order |= 0x40u;
    }
    lfn->attr = FAT32_ATTR_LFN;
    lfn->type = 0;
    lfn->checksum = checksum;
    lfn->zero = 0;

    for (uint32_t part = 0; part < 3; ++part) {
        for (uint32_t i = 0; i < slot_counts[part]; ++i) {
            uint16_t value = 0xffffu;
            if (cursor < name_len) {
                value = name[cursor++];
            } else if (!terminated) {
                value = 0x0000u;
                terminated = 1;
            }
            slots[part][i] = value;
        }
    }
}

int fat32_name_needs_lfn(const char *name, uint8_t short_name[11], uint8_t *need_lfn)
{
    int ret = fat32_make_short_name(name, short_name);
    if (!need_lfn) {
        return -22;
    }
    *need_lfn = 0;
    if (ret == 0) {
        char rendered[RELIEFOS_FS_NAME_LEN];
        uint32_t pos = 0;
        for (uint32_t i = 0; i < 8 && short_name[i] != ' '; ++i) {
            char ch = (char)short_name[i];
            if (ch >= 'A' && ch <= 'Z') {
                ch = (char)(ch - 'A' + 'a');
            }
            if (pos + 1 < sizeof(rendered)) {
                rendered[pos++] = ch;
            }
        }
        if (short_name[8] != ' ' && pos + 1 < sizeof(rendered)) {
            rendered[pos++] = '.';
        }
        for (uint32_t i = 8; i < 11 && short_name[i] != ' '; ++i) {
            char ch = (char)short_name[i];
            if (ch >= 'A' && ch <= 'Z') {
                ch = (char)(ch - 'A' + 'a');
            }
            if (pos + 1 < sizeof(rendered)) {
                rendered[pos++] = ch;
            }
        }
        rendered[pos] = 0;
        *need_lfn = storage_text_eq(name, rendered) ? 0u : 1u;
        return 0;
    }
    *need_lfn = 1;
    return 1;
}

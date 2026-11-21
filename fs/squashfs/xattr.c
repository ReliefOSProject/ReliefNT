/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

#define SQFS_XATTR_OOL 0x100u
#define SQFS_XATTR_NAME_MAX 255u
#define SQFS_XATTR_VALUE_MAX 65536u
#define SQFS_XATTR_LIST_MAX 65536u

struct sqfs_xattr_iter {
    struct sqfs_position pos;
    uint32_t remaining;
};

struct sqfs_xattr {
    struct sqfs_position value;
    uint32_t size, name_size;
    bool trusted;
    char name[SQFS_XATTR_NAME_MAX + 1];
};

static int xattr_position(const struct sqfs_super *fs, uint64_t ref, struct sqfs_position *out)
{
    uint64_t block = ref >> 16;
    uint32_t offset = ref & 0xffffu;
    if (block >= fs->end[SQFS_XATTR] - fs->begin[SQFS_XATTR] || offset >= SQFS_META_BYTES)
        return -EIO;
    *out = (struct sqfs_position){fs->begin[SQFS_XATTR] + block, offset};
    return 0;
}

static int begin_xattrs(struct storage_volume *volume, const struct storage_node *node,
                        struct sqfs_xattr_iter *iter)
{
    struct sqfs_inode inode;
    int rc = sqfs_inode_read(volume, node->entry_id, &inode);
    if (rc || inode.xattr == SQFS_INVALID_INDEX) return rc;
    uint8_t record[16];
    rc = sqfs_index_read(volume, SQFS_XATTR_ID, inode.xattr, record);
    if (rc) return rc;
    uint32_t count = sqfs_u32(record + 8), size = sqfs_u32(record + 12);
    /* The reported unpacked size includes at least a name and its NUL. */
    if (!count || count > size / 2 || count > SQFS_XATTR_LIST_MAX) return -EIO;
    rc = xattr_position(volume->squashfs, sqfs_u64(record), &iter->pos);
    if (!rc) iter->remaining = count;
    return rc;
}

static int next_xattr(struct storage_volume *volume, struct sqfs_xattr_iter *iter,
                      struct sqfs_xattr *attr)
{
    static const char *const prefixes[] = {"user.", "trusted.", "security."};
    uint8_t field[8];
    int rc = sqfs_meta_read(volume, SQFS_XATTR, &iter->pos, field, 4);
    if (rc) return rc;
    uint16_t type = sqfs_u16(field), name_bytes = sqfs_u16(field + 2);
    unsigned namespace = type & 0xffu;
    const char *prefix = namespace < sizeof(prefixes) / sizeof(*prefixes) ? prefixes[namespace] : NULL;
    attr->trusted = namespace == 1;
    attr->name_size = 0;
    if (!name_bytes || name_bytes > SQFS_XATTR_NAME_MAX || (type & ~0x1ffu)) return -EIO;
    if (prefix) {
        size_t prefix_size = __builtin_strlen(prefix);
        if (name_bytes > SQFS_XATTR_NAME_MAX - prefix_size) return -EIO;
        __builtin_memcpy(attr->name, prefix, prefix_size);
        rc = sqfs_meta_read(volume, SQFS_XATTR, &iter->pos, attr->name + prefix_size, name_bytes);
        if (rc) return rc;
        for (unsigned i = 0; i < name_bytes; ++i)
            if (!attr->name[prefix_size + i]) return -EIO;
        attr->name_size = prefix_size + name_bytes + 1;
        attr->name[attr->name_size - 1] = 0;
    } else {
        rc = sqfs_meta_read(volume, SQFS_XATTR, &iter->pos, NULL, name_bytes);
        if (rc) return rc;
    }
    rc = sqfs_meta_read(volume, SQFS_XATTR, &iter->pos, field, 4);
    if (rc) return rc;
    attr->size = sqfs_u32(field);
    if (type & SQFS_XATTR_OOL) {
        if (attr->size != 8) return -EIO;
        rc = sqfs_meta_read(volume, SQFS_XATTR, &iter->pos, field, 8);
        if (!rc) rc = xattr_position(volume->squashfs, sqfs_u64(field), &attr->value);
        if (!rc) rc = sqfs_meta_read(volume, SQFS_XATTR, &attr->value, field, 4);
        if (rc) return rc;
        attr->size = sqfs_u32(field);
    } else {
        if (attr->size > SQFS_XATTR_VALUE_MAX) return -EIO;
        attr->value = iter->pos;
        rc = sqfs_meta_read(volume, SQFS_XATTR, &iter->pos, NULL, attr->size);
        if (rc) return rc;
    }
    if (attr->size > SQFS_XATTR_VALUE_MAX) return -EIO;
    --iter->remaining;
    return 0;
}

int sqfs_getxattr(struct storage_volume *volume, const struct storage_node *node, const char *name,
                  void *value, uint32_t capacity, uint32_t *length)
{
    struct sqfs_xattr_iter iter = {0};
    int rc = begin_xattrs(volume, node, &iter);
    if (rc) return rc;
    while (iter.remaining) {
        struct sqfs_xattr attr;
        rc = next_xattr(volume, &iter, &attr);
        if (rc) return rc;
        if (!attr.name_size || __builtin_strcmp(attr.name, name)) continue;
        if (capacity) {
            if (attr.size > capacity) return -ERANGE;
            rc = sqfs_meta_read(volume, SQFS_XATTR, &attr.value, value, attr.size);
            if (rc) return rc;
        }
        *length = attr.size;
        return 0;
    }
    return -ENODATA;
}

int sqfs_listxattr(struct storage_volume *volume, const struct storage_node *node, void *names,
                   uint32_t capacity, uint32_t *length, bool include_trusted)
{
    struct sqfs_xattr_iter iter = {0};
    int rc = begin_xattrs(volume, node, &iter);
    if (rc) return rc;
    uint32_t size = 0;
    while (iter.remaining) {
        struct sqfs_xattr attr;
        rc = next_xattr(volume, &iter, &attr);
        if (rc) return rc;
        if (!attr.name_size || (attr.trusted && !include_trusted)) continue;
        if (attr.name_size > SQFS_XATTR_LIST_MAX - size) return -E2BIG;
        if (capacity) {
            if (attr.name_size > capacity - size) return -ERANGE;
            __builtin_memcpy((uint8_t *)names + size, attr.name, attr.name_size);
        }
        size += attr.name_size;
    }
    *length = size;
    return 0;
}

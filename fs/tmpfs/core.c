#include <reliefnt/tmpfs.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/time.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/sched.h>
#include <linux/mount.h>
#include <linux/limits.h>

#define TMPFS_PAGE 4096u
#define TMPFS_HASH 64u
struct tmpfs_page {
    struct tmpfs_page *next;
    uint64_t index;
    uint64_t phys;
};
struct tmpfs_entry {
    struct tmpfs_entry *next;
    struct tmpfs_inode *inode;
    uint64_t cookie;
    char name[RELIEFOS_FS_NAME_LEN];
};
struct tmpfs_inode {
    struct tmpfs_inode *next, *parent;
    struct linux_stat_abi stat;
    struct tmpfs_page *pages[TMPFS_HASH];
    struct tmpfs_entry *entries;
    uint64_t next_cookie, refs;
    char *target;
};
struct tmpfs_super {
    struct tmpfs_inode *inodes[TMPFS_HASH], *root;
    uint64_t max_pages, used_pages, max_inodes, used_inodes, flags;
    uint32_t volume, next_ino;
};

static int64_t tmpfs_now(void)
{
    struct reliefos_time_info now;
    return time_wall_clock(&now) == 0 ? (int64_t)now.unix_seconds : 0;
}
static void tmpfs_changed(struct tmpfs_inode *inode, bool data)
{
    inode->stat.ctime_sec = tmpfs_now();
    if (data)
        inode->stat.mtime_sec = inode->stat.ctime_sec;
}
static bool tmpfs_dir(const struct tmpfs_inode *inode)
{
    return (inode->stat.st_mode & LINUX_S_IFMT) == LINUX_S_IFDIR;
}
static struct tmpfs_inode *tmpfs_inode(struct tmpfs_super *fs, uint32_t ino)
{
    for (struct tmpfs_inode *n = fs->inodes[ino % TMPFS_HASH]; n; n = n->next)
        if (n->stat.st_ino == ino)
            return n;
    return NULL;
}
/** @brief Describe one live memory inode without confusing rdev with inode ID.
 * @param fs Borrowed serialized filesystem. @param n Borrowed live inode.
 * @param out Optional metadata output; receives no owned inode hold.
 * @return None. Device inodes retain first_cluster as inode identity and mark
 * their independent stat rdev for device-open resolution. No locks/I/O.
 */
static void tmpfs_node(struct tmpfs_super *fs, struct tmpfs_inode *n, struct storage_node *out)
{
    if (!out)
        return;
    uint32_t mode = n->stat.st_mode & LINUX_S_IFMT;
    *out =
        (struct storage_node){.volume_id = fs->volume,
                              .first_cluster = n->stat.st_ino,
                              .size = n->stat.st_size,
                              .flags = STORAGE_NODE_FLAG_TMPFS | (n == fs->root ? STORAGE_NODE_FLAG_ROOT : 0),
                              .type = mode == LINUX_S_IFDIR    ? RELIEFOS_FS_TYPE_DIR
                                      : mode == LINUX_S_IFCHR || mode == LINUX_S_IFBLK ? RELIEFOS_FS_TYPE_DEVICE
                                      : mode == LINUX_S_IFLNK  ? RELIEFOS_FS_TYPE_SYMLINK
                                      : mode == LINUX_S_IFIFO  ? RELIEFOS_FS_TYPE_FIFO
                                      : mode == LINUX_S_IFSOCK ? RELIEFOS_FS_TYPE_SOCKET
                                                               : RELIEFOS_FS_TYPE_FILE};
    if(mode==LINUX_S_IFCHR || mode==LINUX_S_IFBLK)
        out->flags|=STORAGE_NODE_FLAG_DEV_NODE|STORAGE_NODE_FLAG_RDEV;
}
static void tmpfs_free_inode(struct tmpfs_super *fs, struct tmpfs_inode *n)
{
    struct storage_node node;
    tmpfs_node(fs, n, &node);
    page_cache_invalidate_node(&node);
    for (unsigned i = 0; i < TMPFS_HASH; ++i) {
        struct tmpfs_page *p = n->pages[i];
        while (p) {
            struct tmpfs_page *next = p->next;
            mm_free_page(p->phys);
            kernel_free(p);
            p = next;
            --fs->used_pages;
        }
    }
    struct tmpfs_entry *e = n->entries;
    while (e) {
        struct tmpfs_entry *next = e->next;
        kernel_free(e);
        e = next;
    }
    struct tmpfs_inode **link = &fs->inodes[n->stat.st_ino % TMPFS_HASH];
    while (*link != n)
        link = &(*link)->next;
    *link = n->next;
    kernel_free(n->target);
    kernel_free(n);
    --fs->used_inodes;
}
void tmpfs_destroy(struct tmpfs_super *fs)
{
    if (!fs)
        return;
    for (unsigned i = 0; i < TMPFS_HASH; ++i)
        while (fs->inodes[i])
            tmpfs_free_inode(fs, fs->inodes[i]);
    kernel_free(fs);
}
static int tmpfs_alloc_inode(struct tmpfs_super *fs, uint32_t mode, struct tmpfs_inode **out)
{
    if ((fs->max_inodes && fs->used_inodes >= fs->max_inodes) || !fs->next_ino)
        return -28;
    struct tmpfs_inode *n = kernel_malloc(sizeof(*n));
    if (!n)
        return -12;
    __builtin_memset(n, 0, sizeof(*n));
    n->stat = (struct linux_stat_abi){.st_ino = fs->next_ino++,
                                      .st_dev = fs->volume + 1,
                                      .st_mode = mode,
                                      .st_nlink = (mode & LINUX_S_IFMT) == LINUX_S_IFDIR ? 2 : 1,
                                      .st_blksize = TMPFS_PAGE,
                                      .atime_sec = tmpfs_now()};
    n->stat.mtime_sec = n->stat.ctime_sec = n->stat.atime_sec;
    if (tmpfs_dir(n))
        n->stat.st_size = 40;
    n->next_cookie = 2;
    unsigned hash = n->stat.st_ino % TMPFS_HASH;
    n->next = fs->inodes[hash];
    fs->inodes[hash] = n;
    ++fs->used_inodes;
    *out = n;
    return 0;
}
static int tmpfs_number(const char *text, uint64_t *out, unsigned base, bool suffix, bool percent)
{
    uint64_t value = 0;
    unsigned digits = 0;
    while (*text >= '0' && *text <= '9') {
        unsigned digit = *text++ - '0';
        if (digit >= base || value > (UINT64_MAX - digit) / base)
            return -22;
        value = value * base + digit;
        ++digits;
    }
    if (!digits)
        return -22;
    if (suffix && *text) {
        unsigned shift = 0;
        switch (*text) {
        case 'k':
        case 'K':
            shift = 10;
            break;
        case 'm':
        case 'M':
            shift = 20;
            break;
        case 'g':
        case 'G':
            shift = 30;
            break;
        case 't':
        case 'T':
            shift = 40;
            break;
        case 'p':
        case 'P':
            shift = 50;
            break;
        case 'e':
        case 'E':
            shift = 60;
            break;
        }
        if (shift) {
            if (value > (UINT64_MAX >> shift))
                return -22;
            value <<= shift;
            ++text;
        }
    }
    if (percent && *text == '%') {
        uint64_t ram = mm_total_memory_kib() * 1024;
        if (value && ram > UINT64_MAX / value)
            return -22;
        value = value * ram / 100;
        ++text;
    }
    if (*text)
        return -22;
    *out = value;
    return 0;
}
/**
 * @brief Allocate a regular inode with no directory link under the execution lock.
 * @param fs Required filesystem owning the new inode.
 * @param uid File owner UID.
 * @param gid File owner GID.
 * @param out Required node output receiving one hold, released by tmpfs_drop.
 * @return Zero on success, or negative errno without transferring a hold.
 */
int tmpfs_create_anonymous(struct tmpfs_super *fs, uint32_t uid, uint32_t gid,
                            struct storage_node *out)
{
    if (!fs || !out) return -22;
    struct tmpfs_inode *inode;
    int ret = tmpfs_alloc_inode(fs, LINUX_S_IFREG | 0777, &inode);
    if (ret < 0) return ret;
    inode->stat.st_nlink = 0;
    inode->stat.st_uid = uid;
    inode->stat.st_gid = gid;
    inode->refs = 1;
    tmpfs_node(fs, inode, out);
    return 0;
}

int tmpfs_new(uint32_t volume, uint64_t flags, const char *options, uint32_t uid, uint32_t gid,
              struct tmpfs_super **out)
{
    if (!out)
        return -22;
    *out = NULL;
    struct tmpfs_super *fs = kernel_malloc(sizeof(*fs));
    if (!fs)
        return -12;
    __builtin_memset(fs, 0, sizeof(*fs));
    fs->volume = volume;
    fs->next_ino = 1;
    fs->flags = flags;
    fs->max_pages = fs->max_inodes = mm_total_memory_kib() / 8; /* Half RAM, in pages. */
    uint32_t mode = 01777;
    const char *p = options ? options : "";
    int ret = -22;
    while (*p) {
        char option[128];
        unsigned length = 0;
        while (*p && *p != ',') {
            if (length + 1 == sizeof(option))
                goto fail;
            option[length++] = *p++;
        }
        if (*p)
            ++p;
        option[length] = 0;
        if (!length)
            continue;
        char *value = option;
        while (*value && *value != '=')
            ++value;
        if (!*value)
            goto fail;
        *value++ = 0;
        uint64_t number;
        bool is_mode = !__builtin_strcmp(option, "mode");
        bool is_size = !__builtin_strcmp(option, "size");
        bool is_blocks = !__builtin_strcmp(option, "nr_blocks");
        bool is_inodes = !__builtin_strcmp(option, "nr_inodes");
        if (tmpfs_number(value, &number, is_mode ? 8 : 10, is_size || is_blocks || is_inodes, is_size))
            goto fail;
        if (is_size) {
            if (number > UINT64_MAX - 4095)
                goto fail;
            fs->max_pages = (number + 4095) / 4096;
        } else if (is_blocks) {
            if (number > INT64_MAX)
                goto fail;
            fs->max_pages = number;
        } else if (is_inodes) {
            if (number > UINT32_MAX)
                goto fail;
            fs->max_inodes = number;
        } else if (is_mode) {
            if (number > UINT32_MAX)
                goto fail;
            mode = number & 07777;
        } else if (!__builtin_strcmp(option, "uid")) {
            if (number >= UINT32_MAX)
                goto fail;
            uid = number;
        } else if (!__builtin_strcmp(option, "gid")) {
            if (number >= UINT32_MAX)
                goto fail;
            gid = number;
        } else
            goto fail;
    }
    ret = tmpfs_alloc_inode(fs, LINUX_S_IFDIR | mode, &fs->root);
    if (ret)
        goto fail;
    fs->root->parent = fs->root;
    fs->root->stat.st_uid = uid;
    fs->root->stat.st_gid = gid;
    *out = fs;
    return 0;
fail:
    tmpfs_destroy(fs);
    return ret;
}
static struct tmpfs_entry **tmpfs_find(struct tmpfs_inode *dir, const char *name)
{
    struct tmpfs_entry **e = &dir->entries;
    while (*e && __builtin_strcmp((*e)->name, name))
        e = &(*e)->next;
    return e;
}
static int tmpfs_walk(struct tmpfs_super *fs, const char *path, bool parent, struct tmpfs_inode **out,
                      char *last)
{
    if (!fs || !path || *path != '/')
        return -22;
    struct tmpfs_inode *n = fs->root;
    const char *p = path;
    while (*p) {
        while (*p == '/')
            ++p;
        if (!*p)
            break;
        if (!tmpfs_dir(n))
            return -20;
        char name[RELIEFOS_FS_NAME_LEN];
        unsigned length = 0;
        while (*p && *p != '/') {
            if (length + 1 == sizeof(name))
                return -36;
            name[length++] = *p++;
        }
        name[length] = 0;
        const char *tail = p;
        while (*tail == '/')
            ++tail;
        if (parent && !*tail) {
            if (!__builtin_strcmp(name, ".") || !__builtin_strcmp(name, ".."))
                return -22;
            __builtin_memcpy(last, name, length + 1);
            *out = n;
            return 0;
        }
        if (!__builtin_strcmp(name, "."))
            continue;
        if (!__builtin_strcmp(name, "..")) {
            n = n->parent;
            continue;
        }
        struct tmpfs_entry *e = *tmpfs_find(n, name);
        if (!e)
            return -2;
        n = e->inode;
        /* Global namei resolves links before issuing backend operations. */
        if (*p == '/' && !tmpfs_dir(n))
            return -20;
    }
    if (parent)
        return -16;
    *out = n;
    return 0;
}
int tmpfs_lookup(struct tmpfs_super *fs, const char *path, struct storage_node *out)
{
    struct tmpfs_inode *n;
    int ret = tmpfs_walk(fs, path, false, &n, NULL);
    if (!ret)
        tmpfs_node(fs, n, out);
    return ret;
}
int tmpfs_lookup_child(struct tmpfs_super *fs,uint32_t parent_ino,const char *name,
                         struct storage_node *out)
{
    if (!fs || !name || !name[0] || !out) return -22;
    for (const char *p=name;*p;++p) if (*p=='/') return -22;
    if (!__builtin_strcmp(name,".") || !__builtin_strcmp(name,"..")) return -22;
    struct tmpfs_inode *parent=tmpfs_inode(fs,parent_ino);
    if (!parent) return -2;
    if (!tmpfs_dir(parent)) return -20;
    struct tmpfs_entry *entry=*tmpfs_find(parent,name);
    if (!entry) return -2;
    tmpfs_node(fs,entry->inode,out);return 0;
}

static struct tmpfs_entry *tmpfs_entry_new(struct tmpfs_inode *parent, const char *name,
                                           struct tmpfs_inode *n)
{
    if (parent->next_cookie == UINT64_MAX)
        return NULL;
    struct tmpfs_entry *e = kernel_malloc(sizeof(*e));
    if (!e)
        return NULL;
    *e = (struct tmpfs_entry){.inode = n, .cookie = parent->next_cookie++};
    __builtin_memcpy(e->name, name, __builtin_strlen(name) + 1);
    return e;
}
static void tmpfs_attach(struct tmpfs_inode *parent, struct tmpfs_entry *e)
{
    struct tmpfs_entry **link = &parent->entries;
    while (*link)
        link = &(*link)->next;
    *link = e;
    e->next = NULL;
    parent->stat.st_size += 20;
    if (tmpfs_dir(e->inode)) {
        ++parent->stat.st_nlink;
        e->inode->parent = parent;
    }
    tmpfs_changed(parent, true);
}
/** @brief Create one linked memory inode without reconstructing its parent path.
 * @param fs Borrowed live filesystem under exclusive execution ownership.
 * @param parent_ino Actual linked directory inode. @param name Literal component.
 * @param mode Full supported inode type and permission bits, already authorized.
 * @param target Nonempty symlink target, NULL for all other inode types.
 * @param value Optional owner/group; NULL selects root ownership.
 * @param out Optional borrowed metadata result, never a retained inode hold.
 * @return Zero or negative input/type/readonly/existence/quota/allocation errno.
 * Every failure rolls back before publication. Character/block inodes start
 * at device zero; tmpfs_mknod_child supplies an explicit device number. No
 * VFS routing, I/O, provider lock or reference transfer occurs.
 */
int tmpfs_create_child(struct tmpfs_super *fs,uint32_t parent_ino,const char *name,
                           uint32_t mode,const char *target,const struct reliefos_permissions *value,
                           struct storage_node *out)
{
    if(!fs || !name || !name[0] || !__builtin_strcmp(name,".") || !__builtin_strcmp(name,".."))return -22;
    size_t length=0;
    for(;name[length];++length) {
        if(name[length]=='/')return -22;
        if(length>=RELIEFOS_FS_NAME_LEN-1)return -36;
    }
    if(fs->flags&MS_RDONLY)return -30;
    struct tmpfs_inode *parent=tmpfs_inode(fs,parent_ino),*n;
    if(!parent)return -2;
    if(!tmpfs_dir(parent))return -20;
    if (!parent->stat.st_nlink)
        return -2;
    if (*tmpfs_find(parent, name))
        return -17;
    uint32_t type = mode & LINUX_S_IFMT;
    if (type != LINUX_S_IFDIR && type != LINUX_S_IFREG && type != LINUX_S_IFLNK && type != LINUX_S_IFSOCK &&
        type != LINUX_S_IFIFO && type != LINUX_S_IFCHR && type != LINUX_S_IFBLK)
        return -95;
    if(target && type!=LINUX_S_IFLNK)return -22;
    if (type == LINUX_S_IFLNK && (!target || !*target))
        return -2;
    int ret = tmpfs_alloc_inode(fs, mode, &n);
    if (ret)
        return ret;
    n->parent = parent;
    if(value){n->stat.st_uid=value->uid;n->stat.st_gid=value->gid;}
    if (target) {
        size_t length = __builtin_strlen(target);
        if (length >= LINUX_PATH_MAX) {
            tmpfs_free_inode(fs, n);
            return -36;
        }
        n->target = kernel_malloc(length + 1);
        if (!n->target) {
            tmpfs_free_inode(fs, n);
            return -12;
        }
        __builtin_memcpy(n->target, target, length + 1);
        n->stat.st_size = length;
    }
    struct tmpfs_entry *e = tmpfs_entry_new(parent, name, n);
    if (!e) {
        tmpfs_free_inode(fs, n);
        return -12;
    }
    tmpfs_attach(parent, e);
    tmpfs_node(fs, n, out);
    return 0;
}
/** @brief Create an authorized special inode and assign its independent rdev.
 * @param fs Borrowed serialized filesystem. @param parent_ino Linked directory.
 * @param name Literal component. @param mode S_IFCHR/S_IFBLK plus permissions.
 * @param device Encoded Linux stat device number, never a CPU/hardware pointer.
 * @param value Optional owner/group. @param out Optional borrowed metadata.
 * @return Zero or native creation errno. No fallible work remains after
 * insertion; failures publish nothing, no device driver is invoked or pinned.
 */
int tmpfs_mknod_child(struct tmpfs_super *fs,uint32_t parent_ino,const char *name,
                         uint32_t mode,uint64_t device,const struct reliefos_permissions *value,
                         struct storage_node *out)
{
    uint32_t type=mode&LINUX_S_IFMT;
    if(type!=LINUX_S_IFCHR && type!=LINUX_S_IFBLK)return -22;
    struct storage_node node;int rc=tmpfs_create_child(fs,parent_ino,name,mode,NULL,value,&node);
    if(rc)return rc;
    tmpfs_inode(fs,node.first_cluster)->stat.st_rdev=device;
    if(out)*out=node;return 0;
}
/** @brief Replace one unpublished symlink payload without losing hardlink identity.
 * @param fs Borrowed serialized filesystem. @param ino Shared symlink inode.
 * @param target Borrowed nonempty NUL-terminated target shorter than PATH_MAX.
 * @return Zero or EINVAL/ENOENT/EROFS/ENAMETOOLONG/ENOMEM. Failure retains old
 * bytes/metadata, success changes shared payload and timestamps. Construction
 * must own the entire private filesystem; no VFS routing, locks or I/O occur.
 */
int tmpfs_rewrite_symlink(struct tmpfs_super *fs,uint32_t ino,const char *target)
{
    if(!fs || !target || !target[0])return -22;
    if(fs->flags&MS_RDONLY)return -30;
    struct tmpfs_inode *n=tmpfs_inode(fs,ino);if(!n)return -2;
    if((n->stat.st_mode&LINUX_S_IFMT)!=LINUX_S_IFLNK)return -22;
    size_t length=0;while(length<LINUX_PATH_MAX && target[length])++length;
    if(length>=LINUX_PATH_MAX)return -36;
    char *copy=kernel_malloc(length+1);if(!copy)return -12;
    __builtin_memcpy(copy,target,length+1);kernel_free(n->target);n->target=copy;
    n->stat.st_size=length;tmpfs_changed(n,true);return 0;
}
int tmpfs_create(struct tmpfs_super *fs,const char *path,uint32_t mode,const char *target,
                    struct storage_node *out)
{
    if(!fs || !path)return -22;
    if(fs->flags&MS_RDONLY)return -30;
    struct tmpfs_inode *parent;char name[RELIEFOS_FS_NAME_LEN];
    int ret=tmpfs_walk(fs,path,true,&parent,name);if(ret)return ret;
    return tmpfs_create_child(fs,(uint32_t)parent->stat.st_ino,name,mode,target,NULL,out);
}

static struct tmpfs_page *tmpfs_page_find(struct tmpfs_inode *n, uint64_t index)
{
    for (struct tmpfs_page *p = n->pages[index % TMPFS_HASH]; p; p = p->next)
        if (p->index == index)
            return p;
    return NULL;
}
/** @brief Allocate one zeroed, quota-accounted page without publishing it.
 * @param fs Borrowed serialized filesystem.
 * @param n Borrowed owning inode; block accounting changes on success.
 * @param index Logical page index, not already allocated by this caller.
 * @param out Receives one page owned by the caller until attached or freed.
 * @return Zero or ENOSPC/ENOMEM with accounting unchanged on failure.
 */
static int tmpfs_page_allocate(struct tmpfs_super *fs,struct tmpfs_inode *n,uint64_t index,
                                 struct tmpfs_page **out)
{
    if (fs->max_pages && fs->used_pages >= fs->max_pages) return -28;
    struct tmpfs_page *p=kernel_malloc(sizeof(*p));
    if (!p) return -12;
    p->phys=mm_alloc_page();
    if (!p->phys) {kernel_free(p);return -12;}
    __builtin_memset(mm_phys_to_virt(p->phys),0,TMPFS_PAGE);
    p->index=index;p->next=NULL;
    ++fs->used_pages;n->stat.st_blocks+=TMPFS_PAGE/512;
    *out=p;return 0;
}
/** @brief Find or publish one quota-accounted file page.
 * @param fs Borrowed serialized filesystem.
 * @param n Borrowed live regular inode.
 * @param index Logical page index.
 * @param out Receives a borrowed inode-owned page, no physical retain.
 * @return Zero or allocation errno; existing page contents are unchanged.
 */
static int tmpfs_page_get(struct tmpfs_super *fs,struct tmpfs_inode *n,uint64_t index,
                            struct tmpfs_page **out)
{
    struct tmpfs_page *p=tmpfs_page_find(n,index);
    if (!p) {
        int rc=tmpfs_page_allocate(fs,n,index,&p);if(rc)return rc;
        p->next = n->pages[index % TMPFS_HASH];
        n->pages[index % TMPFS_HASH] = p;
    }
    *out = p;
    return 0;
}
/** @brief Reserve real regular-file pages as one rollback-safe transaction.
 * @param fs Borrowed live filesystem, serialized by its caller.
 * @param ino Existing held or linked inode number.
 * @param mode Zero or Linux FALLOC_FL_KEEP_SIZE (1).
 * @param offset Start byte within signed-off_t range.
 * @param length Positive range length; end must also fit signed off_t.
 * @return Zero or negative type/range/quota/allocation/readonly errno. New
 * pages are private until all allocation succeeds. Old bytes, size and page
 * identities survive failure, and success needs no future allocation for
 * writes into the reserved range. No namespace lookup or lock across work.
 */
int tmpfs_fallocate(struct tmpfs_super *fs,uint32_t ino,uint32_t mode,uint64_t offset,uint64_t length)
{
    if(!fs || !length)return -22;
    if(mode&~1u)return -95;
    if(offset>INT64_MAX || length>(uint64_t)INT64_MAX-offset)return -27;
    if(fs->flags&MS_RDONLY)return -30;
    struct tmpfs_inode *n=tmpfs_inode(fs,ino);
    if(!n)return -2;
    if((n->stat.st_mode&LINUX_S_IFMT)!=LINUX_S_IFREG)return tmpfs_dir(n) ? -21 : -22;
    uint64_t end=offset+length,first=offset/TMPFS_PAGE,last=(end+TMPFS_PAGE-1)/TMPFS_PAGE;
    if(fs->max_pages && last-first>fs->max_pages)return -28;
    struct tmpfs_page *pending=NULL;int rc=0;
    for(uint64_t index=first;index<last;++index) {
        if(tmpfs_page_find(n,index))continue;
        struct tmpfs_page *page=NULL;
        rc=tmpfs_page_allocate(fs,n,index,&page);if(rc)break;
        page->next=pending;pending=page;
    }
    while(pending) {
        struct tmpfs_page *page=pending;pending=page->next;
        if(rc) {
            mm_free_page(page->phys);kernel_free(page);
            --fs->used_pages;n->stat.st_blocks-=TMPFS_PAGE/512;
        } else {
            page->next=n->pages[page->index%TMPFS_HASH];n->pages[page->index%TMPFS_HASH]=page;
        }
    }
    if(!rc) {
        if(!mode && end>(uint64_t)n->stat.st_size)n->stat.st_size=end;
        tmpfs_changed(n,true);
    }
    return rc;
}
int tmpfs_get_page(struct tmpfs_super *fs, uint32_t ino, uint64_t offset, uint64_t *phys)
{
    struct tmpfs_inode *n = tmpfs_inode(fs, ino);
    if (!n)
        return -2;
    if (!phys || offset % TMPFS_PAGE || offset >= (uint64_t)n->stat.st_size ||
        (n->stat.st_mode & LINUX_S_IFMT) != LINUX_S_IFREG)
        return -22;
    struct tmpfs_page *p;
    int ret = tmpfs_page_get(fs, n, offset / TMPFS_PAGE, &p);
    if (ret)
        return ret;
    mm_retain_page(p->phys);
    *phys = p->phys;
    return 0;
}
/** @brief Mark one successful inode access using only the selected mount policy.
 * @param inode Borrowed live inode under execution ownership, required.
 * @param flags Effective attachment/SB flags for this access.
 * @return None. Readonly/noatime/nodiratime suppress changes. Relatime updates
 * for older access time or a full day; strict access uses current whole-second
 * filesystem resolution. Pure metadata update, no allocation/I/O/ownership change.
 */
static void tmpfs_mark_access(struct tmpfs_inode *inode,uint64_t flags)
{
    if(flags&(MS_NOATIME|MS_RDONLY))return;
    if(tmpfs_dir(inode) && (flags&MS_NODIRATIME))return;
    int64_t now=tmpfs_now();
    if(!(flags&MS_RELATIME) || inode->stat.atime_sec<=inode->stat.mtime_sec ||
       inode->stat.atime_sec<=inode->stat.ctime_sec ||
       (now>=inode->stat.atime_sec && (uint64_t)now-(uint64_t)inode->stat.atime_sec>=86400))
        inode->stat.atime_sec=now;
}

/** @brief Read retained RAM pages with attachment-local access-time policy.
 * @param fs Borrowed live filesystem under execution ownership.
 * @param ino Actual regular inode, including a held unlinked inode.
 * @param offset Byte position. @param buffer Destination, NULL only at length0.
 * @param length Requested bytes. @param done Optional completed count output.
 * @param mount_flags Current attachment policy combined with shared readonly.
 * @return Zero or negative inode/type/input errno; no allocation or disk I/O.
 * Returned data and atime belong to this inode, without shared flag mutation.
 */
int tmpfs_read_with_flags(struct tmpfs_super *fs, uint32_t ino, uint64_t offset, void *buffer, uint32_t length,
                            uint32_t *done,uint64_t mount_flags)
{
    uint32_t total = 0;
    if (done)
        *done = 0;
    struct tmpfs_inode *n = tmpfs_inode(fs, ino);
    if (!n)
        return -2;
    if ((n->stat.st_mode & LINUX_S_IFMT) != LINUX_S_IFREG)
        return tmpfs_dir(n) ? -21 : -22;
    if (!buffer && length)
        return -22;
    if (offset >= (uint64_t)n->stat.st_size)
        return 0;
    if (length > (uint64_t)n->stat.st_size - offset)
        length = n->stat.st_size - offset;
    while (total < length) {
        uint64_t pos = offset + total;
        unsigned skip = pos % TMPFS_PAGE, take = TMPFS_PAGE - skip;
        if (take > length - total)
            take = length - total;
        struct tmpfs_page *p = tmpfs_page_find(n, pos / TMPFS_PAGE);
        if (p)
            __builtin_memcpy((uint8_t *)buffer + total, (uint8_t *)mm_phys_to_virt(p->phys) + skip, take);
        else
            __builtin_memset((uint8_t *)buffer + total, 0, take);
        total += take;
    }
    if(length)tmpfs_mark_access(n,mount_flags);
    if (done)
        *done = total;
    return 0;
}
/** @brief Read an anonymous/raw inode using its filesystem's default policy.
 * @param fs Borrowed filesystem under execution ownership, nullable on error.
 * @param ino Retained regular inode. @param offset Byte position.
 * @param buffer Destination, NULL only at length0. @param length Requested bytes.
 * @param done Optional completed count output.
 * @return Zero or negative inode/type/input errno, no allocation or I/O. This
 * entry serves anonymous/raw users; mounted paths pass their own policy above.
 */
int tmpfs_read(struct tmpfs_super *fs,uint32_t ino,uint64_t offset,void *buffer,uint32_t length,uint32_t *done)
{return tmpfs_read_with_flags(fs,ino,offset,buffer,length,done,fs ? fs->flags : 0);}
int tmpfs_write(struct tmpfs_super *fs, uint32_t ino, uint64_t offset, const void *buffer, uint32_t length,
                uint32_t *done)
{
    uint32_t total = 0;
    if (done)
        *done = 0;
    if (fs->flags & MS_RDONLY)
        return -30;
    struct tmpfs_inode *n = tmpfs_inode(fs, ino);
    if (!n)
        return -2;
    if ((n->stat.st_mode & LINUX_S_IFMT) != LINUX_S_IFREG)
        return tmpfs_dir(n) ? -21 : -22;
    if (!buffer && length)
        return -22;
    if (offset > INT64_MAX || length > (uint64_t)INT64_MAX - offset)
        return -27;
    int ret = 0;
    while (total < length) {
        uint64_t pos = offset + total, index = pos / TMPFS_PAGE;
        unsigned skip = pos % TMPFS_PAGE, take = TMPFS_PAGE - skip;
        if (take > length - total)
            take = length - total;
        struct tmpfs_page *p;
        ret = tmpfs_page_get(fs, n, index, &p);
        if (ret)
            break;
        __builtin_memcpy((uint8_t *)mm_phys_to_virt(p->phys) + skip, (const uint8_t *)buffer + total, take);
        total += take;
    }
    if (total) {
        if (offset + total > (uint64_t)n->stat.st_size)
            n->stat.st_size = offset + total;
        tmpfs_changed(n, true);
    }
    if (done)
        *done = total;
    return ret;
}
int tmpfs_truncate(struct tmpfs_super *fs, uint32_t ino, uint64_t length)
{
    if (fs->flags & MS_RDONLY)
        return -30;
    struct tmpfs_inode *n = tmpfs_inode(fs, ino);
    if (!n)
        return -2;
    if ((n->stat.st_mode & LINUX_S_IFMT) != LINUX_S_IFREG)
        return tmpfs_dir(n) ? -21 : -22;
    if (length > INT64_MAX)
        return -27;
    if (length <= (uint64_t)n->stat.st_size) {
        struct storage_node node;
        tmpfs_node(fs, n, &node);
        n->stat.st_size = length;
        /* Revoke PTEs, including COW copies, before dropping file-page refs. */
        sched_truncate_file_mappings(&node, length);
        for (unsigned i = 0; i < TMPFS_HASH; ++i) {
            struct tmpfs_page **link = &n->pages[i];
            while (*link) {
                struct tmpfs_page *p = *link;
                if (p->index >= (length + TMPFS_PAGE - 1) / TMPFS_PAGE) {
                    *link = p->next;
                    mm_free_page(p->phys);
                    kernel_free(p);
                    --fs->used_pages;
                    n->stat.st_blocks -= TMPFS_PAGE / 512;
                } else {
                    if (p->index == length / TMPFS_PAGE && length % TMPFS_PAGE)
                        __builtin_memset((uint8_t *)mm_phys_to_virt(p->phys) + length % TMPFS_PAGE, 0,
                                         TMPFS_PAGE - length % TMPFS_PAGE);
                    link = &p->next;
                }
            }
        }
    }
    n->stat.st_size = length;
    tmpfs_changed(n, true);
    return 0;
}
int tmpfs_hold(struct tmpfs_super *fs, uint32_t ino)
{
    struct tmpfs_inode *n = tmpfs_inode(fs, ino);
    if (!n)
        return -2;
    ++n->refs;
    return 0;
}
void tmpfs_drop(struct tmpfs_super *fs, uint32_t ino)
{
    struct tmpfs_inode *n = tmpfs_inode(fs, ino);
    if (!n || !n->refs)
        return;
    --n->refs;
    if (!n->refs && !n->stat.st_nlink)
        tmpfs_free_inode(fs, n);
}
static void tmpfs_remove(struct tmpfs_super *fs, struct tmpfs_inode *parent, struct tmpfs_entry **link)
{
    struct tmpfs_entry *e = *link;
    struct tmpfs_inode *n = e->inode;
    *link = e->next;
    kernel_free(e);
    parent->stat.st_size -= 20;
    if (tmpfs_dir(n)) {
        --parent->stat.st_nlink;
        n->stat.st_nlink = 0;
    } else {
        if (n->stat.st_nlink > 1)
            --fs->used_inodes;
        --n->stat.st_nlink;
    }
    tmpfs_changed(parent, true);
    tmpfs_changed(n, false);
    if (!n->refs && !n->stat.st_nlink)
        tmpfs_free_inode(fs, n);
}
/** @brief Validate one mutation component and select its exact parent inode.
 * @param fs Borrowed live filesystem under execution ownership.
 * @param ino Parent inode number whose backing lifetime the caller retains.
 * @param name Single non-special component.
 * @param out Borrowed directory pointer on success.
 * @return Zero or negative errno, without allocation or reference changes.
 */
static int tmpfs_mutation_parent(struct tmpfs_super *fs,uint32_t ino,const char *name,struct tmpfs_inode **out)
{
    if(!fs || !name || !name[0] || !__builtin_strcmp(name,".") || !__builtin_strcmp(name,".."))return -22;
    for(size_t i=0;name[i];++i) {
        if(name[i]=='/')return -22;
        if(i>=RELIEFOS_FS_NAME_LEN-1)return -36;
    }
    struct tmpfs_inode *parent=tmpfs_inode(fs,ino);
    if(!parent || !parent->stat.st_nlink)return -2;
    if(!tmpfs_dir(parent))return -20;
    *out=parent;return 0;
}
int tmpfs_unlink_child(struct tmpfs_super *fs,uint32_t parent_ino,const char *name,bool directory)
{
    if (!fs)return -22;
    if (fs->flags & MS_RDONLY)
        return -30;
    struct tmpfs_inode *parent;
    int ret = tmpfs_mutation_parent(fs,parent_ino,name,&parent);
    if (ret)
        return ret;
    struct tmpfs_entry **link = tmpfs_find(parent, name);
    if (!*link)
        return -2;
    struct tmpfs_inode *n = (*link)->inode;
    if (tmpfs_dir(n) != directory)
        return directory ? -20 : -21;
    if (directory && n->entries)
        return -39;
    tmpfs_remove(fs, parent, link);
    return 0;
}
int tmpfs_unlink(struct tmpfs_super *fs,const char *path,bool directory)
{
    if(!fs)return -22;
    if(fs->flags&MS_RDONLY)return -30;
    char name[RELIEFOS_FS_NAME_LEN];struct tmpfs_inode *parent;
    int ret=tmpfs_walk(fs,path,true,&parent,name);
    return ret ? ret : tmpfs_unlink_child(fs,(uint32_t)parent->stat.st_ino,name,directory);
}
int tmpfs_link_child(struct tmpfs_super *fs,uint32_t ino,uint32_t parent_ino,const char *name)
{
    if(!fs)return -22;
    if (fs->flags & MS_RDONLY)
        return -30;
    struct tmpfs_inode *parent;
    int ret=tmpfs_mutation_parent(fs,parent_ino,name,&parent);
    if (ret)
        return ret;
    struct tmpfs_inode *n=tmpfs_inode(fs,ino);
    if(!n || !n->stat.st_nlink)return -2;
    if (tmpfs_dir(n))
        return -1;
    if (*tmpfs_find(parent, name))
        return -17;
    if (n->stat.st_nlink >= UINT32_MAX)
        return -31;
    /* tmpfs pins dentries: Linux charges additional hardlinks to nr_inodes. */
    if (fs->max_inodes && fs->used_inodes >= fs->max_inodes)
        return -28;
    struct tmpfs_entry *e = tmpfs_entry_new(parent, name, n);
    if (!e)
        return -12;
    ++fs->used_inodes;
    ++n->stat.st_nlink;
    tmpfs_attach(parent, e);
    tmpfs_changed(n, false);
    return 0;
}
int tmpfs_link(struct tmpfs_super *fs,const char *old_path,const char *new_path)
{
    if(!fs)return -22;
    if(fs->flags&MS_RDONLY)return -30;
    char name[RELIEFOS_FS_NAME_LEN];struct tmpfs_inode *node,*parent;
    int ret=tmpfs_walk(fs,old_path,false,&node,NULL);
    if(!ret && tmpfs_dir(node))ret=-1;
    if(!ret)ret=tmpfs_walk(fs,new_path,true,&parent,name);
    return ret ? ret : tmpfs_link_child(fs,(uint32_t)node->stat.st_ino,(uint32_t)parent->stat.st_ino,name);
}
int tmpfs_rename_child(struct tmpfs_super *fs,uint32_t old_parent_ino,const char *old_name,
                         uint32_t new_parent_ino,const char *new_name)
{
    if(!fs)return -22;
    if (fs->flags & MS_RDONLY)
        return -30;
    struct tmpfs_inode *old_parent, *new_parent;
    int ret=tmpfs_mutation_parent(fs,old_parent_ino,old_name,&old_parent);
    if (ret)
        return ret;
    ret=tmpfs_mutation_parent(fs,new_parent_ino,new_name,&new_parent);
    if (ret)
        return ret;
    struct tmpfs_entry **old = tmpfs_find(old_parent, old_name), **dest = tmpfs_find(new_parent, new_name);
    if (!*old)
        return -2;
    struct tmpfs_inode *n = (*old)->inode;
    if (*dest && (*dest)->inode == n)
        return 0;
    if (tmpfs_dir(n)) {
        for (struct tmpfs_inode *p = new_parent;; p = p->parent) {
            if (p == n)
                return -22;
            if (p == fs->root)
                break;
        }
    }
    if (*dest) {
        if (tmpfs_dir((*dest)->inode) != tmpfs_dir(n))
            return tmpfs_dir(n) ? -20 : -21;
        if (tmpfs_dir(n) && (*dest)->inode->entries)
            return -39;
    }
    /* Allocate before altering either directory, so ENOMEM is atomic. */
    struct tmpfs_entry *replacement = tmpfs_entry_new(new_parent, new_name, n);
    if (!replacement)
        return -12;
    if (*dest)
        tmpfs_remove(fs, new_parent, dest);
    old = tmpfs_find(old_parent, old_name);
    struct tmpfs_entry *e = *old;
    *old = e->next;
    kernel_free(e);
    old_parent->stat.st_size -= 20;
    if (tmpfs_dir(n))
        --old_parent->stat.st_nlink;
    tmpfs_attach(new_parent, replacement);
    tmpfs_changed(old_parent, true);
    tmpfs_changed(n, false);
    return 0;
}
int tmpfs_rename(struct tmpfs_super *fs,const char *old_path,const char *new_path)
{
    if(!fs)return -22;
    if(fs->flags&MS_RDONLY)return -30;
    char old_name[RELIEFOS_FS_NAME_LEN],new_name[RELIEFOS_FS_NAME_LEN];
    struct tmpfs_inode *old_parent,*new_parent;
    int ret=tmpfs_walk(fs,old_path,true,&old_parent,old_name);
    if(!ret)ret=tmpfs_walk(fs,new_path,true,&new_parent,new_name);
    return ret ? ret : tmpfs_rename_child(fs,(uint32_t)old_parent->stat.st_ino,old_name,
        (uint32_t)new_parent->stat.st_ino,new_name);
}
/** @brief Return one real directory entry with the selected attachment's policy.
 * @param fs Borrowed live filesystem under execution ownership.
 * @param ino Linked directory inode. @param offset Required opaque cursor.
 * @param out Required entry destination. @param mount_flags Effective access flags.
 * @return One entry, zero EOF or negative inode/type errno. Successful entries
 * mark atime unless readonly/noatime/nodiratime; no allocation/I/O or flag mutation.
 */
int tmpfs_readdir_with_flags(struct tmpfs_super *fs, uint32_t ino, uint64_t *offset, struct reliefos_dir_entry *out,
                               uint64_t mount_flags)
{
    struct tmpfs_inode *n = tmpfs_inode(fs, ino);
    if (!n)
        return -2;
    if (!tmpfs_dir(n))
        return -20;
    if (!n->stat.st_nlink)
        return -2;
    struct tmpfs_inode *item;
    const char *name;
    if (*offset < 2) {
        name = *offset ? ".." : ".";
        item = *offset ? n->parent : n;
        ++*offset;
    } else {
        struct tmpfs_entry *e = n->entries;
        while (e && e->cookie < *offset)
            e = e->next;
        if (!e)
            return 0;
        item = e->inode;
        name = e->name;
        *offset = e->cookie + 1;
    }
    struct storage_node node;
    tmpfs_node(fs, item, &node);
    __builtin_memset(out, 0, sizeof(*out));
    out->type = node.type;
    __builtin_memcpy(out->name, name, __builtin_strlen(name) + 1);
    tmpfs_mark_access(n,mount_flags);
    return 1;
}
/** @brief Enumerate a raw directory using its filesystem's default policy.
 * @param fs Borrowed live filesystem under execution ownership.
 * @param ino Linked directory inode. @param offset Required cursor.
 * @param out Required entry output.
 * @return One entry, zero EOF or negative inode/type errno. No allocation/I/O;
 * mounted paths use the explicit-policy entry above. No ownership transfer.
 */
int tmpfs_readdir(struct tmpfs_super *fs,uint32_t ino,uint64_t *offset,struct reliefos_dir_entry *out)
{return tmpfs_readdir_with_flags(fs,ino,offset,out,fs ? fs->flags : 0);}
int tmpfs_readlink(struct tmpfs_super *fs, uint32_t ino, void *buffer, uint32_t length, uint32_t *done)
{
    if (done)
        *done = 0;
    struct tmpfs_inode *n = tmpfs_inode(fs, ino);
    if (!n)
        return -2;
    if ((n->stat.st_mode & LINUX_S_IFMT) != LINUX_S_IFLNK)
        return -22;
    if (length > (uint64_t)n->stat.st_size)
        length = n->stat.st_size;
    if (length)
        __builtin_memcpy(buffer, n->target, length);
    if (done)
        *done = length;
    return 0;
}
int tmpfs_stat(struct tmpfs_super *fs, uint32_t ino, struct linux_stat_abi *out)
{
    struct tmpfs_inode *n = tmpfs_inode(fs, ino);
    if (!n)
        return -2;
    *out = n->stat;
    return 0;
}
int tmpfs_permissions(struct tmpfs_super *fs, uint32_t ino, struct reliefos_permissions *value, bool write)
{
    struct tmpfs_inode *n = tmpfs_inode(fs, ino);
    if (!n)
        return -2;
    if (write) {
        if (fs->flags & MS_RDONLY)
            return -30;
        n->stat.st_mode = (n->stat.st_mode & LINUX_S_IFMT) | (value->mode & 07777);
        n->stat.st_uid = value->uid;
        n->stat.st_gid = value->gid;
        tmpfs_changed(n, false);
    } else
        *value = (struct reliefos_permissions){n->stat.st_mode & 07777, n->stat.st_uid, n->stat.st_gid};
    return 0;
}
int tmpfs_utimens(struct tmpfs_super *fs, uint32_t ino, int64_t atime, int64_t mtime, bool set_atime,
                  bool set_mtime)
{
    struct tmpfs_inode *n = tmpfs_inode(fs, ino);
    if (!n)
        return -2;
    if (fs->flags & MS_RDONLY)
        return -30;
    if (set_atime)
        n->stat.atime_sec = atime;
    if (set_mtime)
        n->stat.mtime_sec = mtime;
    if (set_atime || set_mtime)
        tmpfs_changed(n, false);
    return 0;
}
void tmpfs_statfs(struct tmpfs_super *fs, struct linux_statfs_abi *out)
{
    *out = (struct linux_statfs_abi){
        .f_type = 0x01021994,
        .f_bsize = TMPFS_PAGE,
        .f_frsize = TMPFS_PAGE,
        .f_blocks = fs->max_pages,
        .f_bfree = fs->max_pages ? fs->max_pages - fs->used_pages : 0,
        .f_bavail = fs->max_pages ? fs->max_pages - fs->used_pages : 0,
        .f_files = fs->max_inodes,
        .f_ffree = fs->max_inodes ? fs->max_inodes - fs->used_inodes : 0,
        .f_namelen = RELIEFOS_FS_NAME_LEN - 1,
        .f_flags = LINUX_ST_VALID |
                   (fs->flags & (MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | MS_NODIRATIME)) |
                   ((fs->flags & MS_RELATIME) ? LINUX_ST_RELATIME : 0)};
    out->f_fsid[0] = fs->volume + 1;
}
void tmpfs_set_flags(struct tmpfs_super *fs, uint64_t flags) { fs->flags = flags; }

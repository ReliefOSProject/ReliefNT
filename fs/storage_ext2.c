/* Compatibility names for existing callers and regression fixtures. All file,
 * inode, allocation and namespace operations use the native ext-family backend.
 * The raw-block cache helpers below remain solely for legacy transport tests. */
static __attribute__((unused)) uint64_t ext2_inode_size(const struct ext2_inode *inode)
{
    return (uint64_t)inode->size_lo |
        ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG ? (uint64_t)inode->size_high << 32 : 0);
}
static __attribute__((unused)) uint32_t ext2_node_type(const struct ext2_inode *inode)
{
    if ((inode->mode & EXT2_S_IFMT) == LINUX_S_IFIFO) return RELIEFOS_FS_TYPE_FIFO;
    if ((inode->mode & EXT2_S_IFMT) == LINUX_S_IFSOCK) return RELIEFOS_FS_TYPE_SOCKET;
    if ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFLNK) return RELIEFOS_FS_TYPE_SYMLINK;
    return (inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE;
}
static uint32_t ext2_read_recovery_logs;
static __attribute__((unused)) int ext2_read_sectors_resilient(uint64_t lba, uint32_t sectors,
                                       void *buffer)
{
    uint8_t *dst = (uint8_t *)buffer;
    int ret = storage_read_sectors(lba, sectors, buffer);
    if (ret >= 0 || ret == -RELIEFOS_EAGAIN) {
        return ret;
    }
    if (ext2_read_recovery_logs < 8u) {
        console_printf("[reliefnt] ext2 bulk read failed lba=%llu sectors=%u ret=%d; "
                       "retrying sectors\n", (unsigned long long)lba, sectors, ret);
        ++ext2_read_recovery_logs;
    }
    for (uint32_t i = 0; i < sectors; ++i) {
        ret = storage_read_sectors(lba + i, 1u, dst + i * SECTOR_SIZE);
        if (ret < 0) {
            if (ret == -RELIEFOS_EAGAIN) {
                return ret;
            }
            if (ext2_read_recovery_logs < 8u) {
                console_printf("[reliefnt] ext2 sector read failed lba=%llu ret=%d\n",
                               (unsigned long long)(lba + i), ret);
                ++ext2_read_recovery_logs;
            }
            return ret;
        }
    }
    if (ext2_read_recovery_logs < 8u) {
        console_printf("[reliefnt] ext2 sector fallback recovered lba=%llu sectors=%u\n",
                       (unsigned long long)lba, sectors);
        ++ext2_read_recovery_logs;
    }
    return 0;
}
static __attribute__((unused)) int ext2_read_block(uint32_t block, void *buffer)
{
    uint32_t sectors;
    uint32_t cache_sectors;
    uint64_t lba;
    uint64_t cache_end;
    uint64_t remaining_sectors;
    uint32_t cache_offset;
    int ret;
    if (!buffer || !g_storage.ext2_block_size || block >= g_storage.ext2_blocks_count) {
        return -22;
    }
    sectors = g_storage.ext2_block_size / SECTOR_SIZE;
    lba = g_storage.ext2_start_lba + (uint64_t)block * sectors;
    if (ext2_cache_read(lba, sectors, buffer)) return 0;
    cache_end = storage_read_cache.first_lba + storage_read_cache.sector_count;
    if (storage_read_cache.valid && storage_read_cache.volume == g_active_volume &&
        lba >= storage_read_cache.first_lba && lba + sectors <= cache_end) {
        cache_offset = (uint32_t)(lba - storage_read_cache.first_lba) * SECTOR_SIZE;
        storage_memcpy(buffer, storage_read_cache_data + cache_offset,
                       g_storage.ext2_block_size);
        ext2_cache_store(lba, sectors, buffer);
        return 0;
    }

    /* Header-heavy workloads such as TinyCC open thousands of small files.
     * Fill the same bounded cache used by FAT32, but index it by physical LBA
     * so ext2 direct and indirect blocks need no filesystem-specific cache. */
    remaining_sectors = (uint64_t)(g_storage.ext2_blocks_count - block) * sectors;
    cache_sectors = remaining_sectors > STORAGE_READAHEAD_SECTORS
        ? STORAGE_READAHEAD_SECTORS : (uint32_t)remaining_sectors;
    storage_read_cache.valid = 0;
    ret = ext2_read_sectors_resilient(lba, cache_sectors, storage_read_cache_data);
    if (ret < 0) {
        return ret;
    }
    storage_read_cache.volume = g_active_volume;
    storage_read_cache.first_lba = lba;
    storage_read_cache.sector_count = cache_sectors;
    storage_read_cache.valid = 1;
    storage_memcpy(buffer, storage_read_cache_data, g_storage.ext2_block_size);
    ext2_cache_store(lba, sectors, buffer);
    return 0;
}
static __attribute__((unused)) int ext2_write_block(uint32_t block, const void *buffer)
{
    uint32_t sectors;
    uint64_t lba;
    int ret;
    if (!buffer || !g_storage.ext2_block_size || block >= g_storage.ext2_blocks_count) {
        return -30;
    }
    sectors = g_storage.ext2_block_size / SECTOR_SIZE;
    lba = g_storage.ext2_start_lba + (uint64_t)block * sectors;
    ret = storage_write_sectors(lba, sectors, buffer);
    if (!ret) ext2_cache_store(lba, sectors, buffer);
    return ret;
}
static __attribute__((unused)) uint32_t ext2_free_bitmap_bit(const uint8_t *bitmap, uint32_t count, uint32_t hint)
{
    if (hint >= count) hint = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        uint32_t end = pass ? hint : count;
        for (uint32_t bit = pass ? 0 : hint; bit < end;) {
            if (!(bit & 7u) && bitmap[bit / 8u] == 0xffu) {
                bit += 8u;
                continue;
            }
            if (!(bitmap[bit / 8u] & (1u << (bit & 7u)))) return bit;
            ++bit;
        }
    }
    return count;
}

static __attribute__((unused)) int ext2_mount(void)
{ ext2_cache_reset(g_active_volume); return storage_ext4_mount(&g_storage); }
static __attribute__((unused)) int ext2_group_desc(uint32_t group,struct ext2_group_desc *out)
{
    struct storage_ext4_group_view g; int ret=storage_ext4_read_group(&g_storage,group,&g); if (ret<0) return ret;
    *out=(struct ext2_group_desc){.block_bitmap=g.block_bitmap,.inode_bitmap=g.inode_bitmap,
        .inode_table=g.inode_table,.free_blocks_count=g.free_blocks_count,.free_inodes_count=g.free_inodes_count,
        .used_dirs_count=g.used_dirs_count}; return 0;
}
static __attribute__((unused)) int ext2_read_inode(uint32_t ino,struct ext2_inode *out)
{
    struct ext4_inode_view in; int ret=storage_ext4_read_inode(&g_storage,ino,&in);
    if (ret==-2) { storage_memzero(out,sizeof(*out)); return 0; }
    if (ret<0) return ret;
    storage_memzero(out,sizeof(*out)); out->mode=in.mode; out->uid=in.uid; out->gid=in.gid;
    out->size_lo=in.size; out->size_high=in.size>>32; out->blocks_512=in.blocks;
    out->links_count=in.links_count; out->flags=in.flags; out->generation=in.generation;
    out->atime=in.atime; out->mtime=in.mtime; out->ctime=in.ctime; out->dtime=in.dtime; out->file_acl=in.file_acl;
    storage_memcpy(out->block,in.i_block_raw,60); ext4_put_le16(out->osd2,in.blocks>>32);
    ext4_put_le16(out->osd2+2,in.file_acl>>32); ext4_put_le16(out->osd2+4,in.uid>>16);
    ext4_put_le16(out->osd2+6,in.gid>>16); return 0;
}
static __attribute__((unused)) int ext2_write_inode(uint32_t ino,const struct ext2_inode *raw)
{
    struct storage_ext4_handle h; int ret=storage_ext4_journal_start(&g_storage,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
    struct ext4_inode_view in; ret=storage_ext4_read_inode(&g_storage,ino,&in);
    if (!ret) {
        in.mode=raw->mode; in.uid=raw->uid|((uint32_t)ext4_get_le16(raw->osd2+4)<<16);
        in.gid=raw->gid|((uint32_t)ext4_get_le16(raw->osd2+6)<<16);
        in.size=raw->size_lo|((uint64_t)raw->size_high<<32);
        in.blocks=raw->blocks_512|((uint64_t)ext4_get_le16(raw->osd2)<<32);
        in.file_acl=raw->file_acl|((uint64_t)ext4_get_le16(raw->osd2+2)<<32);
        in.links_count=raw->links_count; in.flags=raw->flags; in.generation=raw->generation;
        in.atime=raw->atime; in.mtime=raw->mtime; in.ctime=raw->ctime; in.dtime=raw->dtime;
        storage_memcpy(in.i_block_raw,raw->block,60); ret=storage_ext4_write_inode(&g_storage,ino,&in);
    }
    return ed_stop(&h,ret);
}
static __attribute__((unused)) int ext2_lookup_path(const char *path,struct storage_node *out)
{ return storage_ext4_ops.lookup(&g_storage,path,out); }
static __attribute__((unused)) int ext2_read_node(const struct storage_node *n,uint64_t off,void *b,uint32_t len,uint32_t *done)
{ return storage_ext4_ops.read(&g_storage,n,off,b,len,done); }
static __attribute__((unused)) int ext2_write_node(const struct storage_node *n,uint64_t off,const void *b,uint32_t len,uint32_t *done)
{ return storage_ext4_ops.write(&g_storage,n,off,b,len,done); }
static __attribute__((unused)) int ext2_truncate_file(const struct storage_node *n,uint64_t len)
{ return storage_ext4_ops.truncate(&g_storage,n,len); }
static __attribute__((unused)) int ext2_write_file(const char *p,const void *b,uint32_t len)
{ return storage_ext4_replace(&g_storage,p,b,len); }
static __attribute__((unused)) int ext2_mkdir(const char *p) { return storage_ext4_ops.mkdir(&g_storage,p); }
static __attribute__((unused)) int ext2_unlink(const char *p) { return storage_ext4_ops.unlink(&g_storage,p); }
static __attribute__((unused)) int ext2_rmdir(const char *p) { return storage_ext4_ops.rmdir(&g_storage,p); }
static __attribute__((unused)) int ext2_link(const char *a,const char *b) { return storage_ext4_ops.link(&g_storage,a,b); }
static __attribute__((unused)) int ext2_rename(const char *a,const char *b) { return storage_ext4_ops.rename(&g_storage,a,b); }
static __attribute__((unused)) int ext2_symlink(const char *a,const char *b) { return storage_ext4_ops.symlink(&g_storage,a,b); }
static __attribute__((unused)) int ext2_symlink_read(const struct storage_node *n,char *b,uint32_t len,uint32_t *done)
{ return storage_ext4_ops.readlink(&g_storage,n,b,len,done); }
static __attribute__((unused)) int ext2_mark_special(const char *p,const struct storage_node *n,uint32_t mode)
{ return storage_ext4_mark_special(&g_storage,p,n,mode); }
static __attribute__((unused)) int ext2_destroy_inode(uint32_t ino,uint8_t directory)
{
    (void)directory; struct ext4_inode_view in; int ret=storage_ext4_read_inode(&g_storage,ino,&in); if (ret<0) return ret;
    struct storage_node node; ev_node(&g_storage,ino,&in,&node); page_cache_invalidate_node(&node);
    return storage_ext4_destroy_inode(&g_storage,ino);
}

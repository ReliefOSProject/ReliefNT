/* Native ext-family inode views, reads and range I/O. */
#include "../../block/core/storage_internal.h"
/** @brief Convert a Linux ext-family mode to the existing native node class.
 * @param mode Borrowed inode type/mode bits.
 * @return Directory/link/FIFO/socket class, or the legacy regular-file default.
 * Pure conversion with no ownership, allocation, locking or I/O.
 */
uint32_t storage_ext4_inode_type(uint16_t mode)
{
    switch (mode&0170000) { case 0040000:return RELIEFOS_FS_TYPE_DIR;
        case 0120000:return RELIEFOS_FS_TYPE_SYMLINK; case 0010000:return RELIEFOS_FS_TYPE_FIFO;
        case 0140000:return RELIEFOS_FS_TYPE_SOCKET; default:return RELIEFOS_FS_TYPE_FILE; }
}
/** @brief Translate an already available inode view without another inode read.
 * @param v Borrowed mounted volume supplying instance/generation identity.
 * @param ino Native inode number from that volume.
 * @param in Borrowed decoded or newly constructed inode view.
 * @param out Optional node metadata output; no inode reference is transferred.
 * Pure conversion; caller retains its volume and serializes publication.
 */
void storage_ext4_node_from_inode(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in,struct storage_node *out)
{
    if (out) *out=(struct storage_node){.type=storage_ext4_inode_type(in->mode),.flags=STORAGE_NODE_FLAG_EXT_FAMILY|
        (ino==2?STORAGE_NODE_FLAG_ROOT:0),.first_cluster=ino,.volume_id=v->volume_id,.size=in->size,
        .mount_generation=v->mount_generation,.inode_generation=in->generation};
}

/* Preserve inode extension fields, inline xattrs and OS-dependent bytes. The
 * caller owns a transaction; newly allocated inodes are zeroed by ialloc. */
int storage_ext4_write_inode(struct storage_volume *v, uint64_t ino,
                             const struct ext4_inode_view *in)
{
    if (!v || !in || !ino || ino > v->ext4.inodes_count) return -RELIEFOS_EINVAL;
    struct storage_ext4_group_view g;
    int ret = storage_ext4_read_group(v, (ino-1)/v->ext4.inodes_per_group, &g);
    if (ret < 0) return ret;
    uint32_t bs=v->ext4.block_size, size=v->ext4.inode_size;
    uint64_t at=((ino-1)%v->ext4.inodes_per_group)*size;
    uint64_t block=g.inode_table+at/bs;
    uint32_t off=at%bs, first=size < bs-off ? size : bs-off;
    uint8_t *raw=kernel_malloc(size), *data;
    if (!raw) return -RELIEFOS_ENOMEM;
    ret=storage_ext4_cache_get(v,block,&data,false);
    if (ret<0) goto out;
    storage_memcpy(raw,data+off,first);
    if (first<size) {
        ret=storage_ext4_cache_get(v,block+1,&data,false); if (ret<0) goto out;
        storage_memcpy(raw+first,data,size-first);
    }
    uint64_t blocks=in->blocks;
    if (in->flags & EXT4_HUGE_FILE_FL) blocks/=bs/512;
    if (blocks>>48 || in->file_acl>>48) { ret=-RELIEFOS_EFBIG; goto out; }
    ext4_put_le16(raw,in->mode); ext4_put_le16(raw+2,in->uid);
    ext4_put_le32(raw+4,in->size); ext4_put_le32(raw+108,in->size>>32);
    ext4_put_le32(raw+8,in->atime); ext4_put_le32(raw+12,in->ctime);
    ext4_put_le32(raw+16,in->mtime); ext4_put_le32(raw+20,in->dtime);
    ext4_put_le16(raw+24,in->gid); ext4_put_le16(raw+26,in->links_count);
    ext4_put_le32(raw+28,blocks); ext4_put_le32(raw+32,in->flags);
    storage_memcpy(raw+40,in->i_block_raw,60);
    ext4_put_le32(raw+100,in->generation); ext4_put_le32(raw+104,in->file_acl);
    ext4_put_le16(raw+116,blocks>>32); ext4_put_le16(raw+118,in->file_acl>>32);
    ext4_put_le16(raw+120,in->uid>>16); ext4_put_le16(raw+122,in->gid>>16);
    if (size>128) ext4_put_le16(raw+128,in->extra_isize);
    ret=storage_ext4_update_inode_checksum(raw,size,ino,in->generation,&v->ext4.super_view);
    if (ret<0) goto out;
    ret=storage_ext4_cache_get(v,block,&data,true); if (ret<0) goto out;
    storage_memcpy(data+off,raw,first);
    ret=storage_ext4_cache_mark_dirty(v,block); if (ret<0) goto out;
    if (first<size) {
        ret=storage_ext4_cache_get(v,block+1,&data,true); if (ret<0) goto out;
        storage_memcpy(data,raw+first,size-first);
        ret=storage_ext4_cache_mark_dirty(v,block+1);
    }
out:
    kernel_free(raw); return ret;
}

int storage_ext4_read_inode(struct storage_volume *v, uint64_t ino, struct ext4_inode_view *out)
{
    struct storage_ext4_group_view group;
    if (!v || !out || !ino || ino > v->ext4.inodes_count || !v->ext4.inodes_per_group ||
        v->ext4.inode_size < 128 || v->ext4.inode_size > EXT4_MAX_BLOCK_SIZE ||
        !v->ext4.block_size) return -RELIEFOS_EINVAL;
    int ret = storage_ext4_read_group(v, (ino - 1) / v->ext4.inodes_per_group, &group);
    if (ret < 0) return ret;
    uint64_t offset = ((ino - 1) % v->ext4.inodes_per_group) * v->ext4.inode_size;
    uint64_t block = group.inode_table + offset / v->ext4.block_size;
    uint32_t inside = offset % v->ext4.block_size, size = v->ext4.inode_size;
    uint8_t *data, *owned = NULL;
    ret = storage_ext4_cache_get(v, block, &data, false);
    if (ret < 0) return ret;
    const uint8_t *raw = data + inside;
    if (inside + size > v->ext4.block_size) {
        uint32_t first = v->ext4.block_size - inside;
        owned = kernel_malloc(size);
        if (!owned) return -RELIEFOS_ENOMEM;
        storage_memcpy(owned, raw, first);
        ret = storage_ext4_cache_get(v, block + 1, &data, false);
        if (ret < 0) { kernel_free(owned); return ret; }
        storage_memcpy(owned + first, data, size - first); raw = owned;
    }
    ret = storage_ext4_verify_inode_checksum(raw, size, (uint32_t)ino,
                                            ext4_get_le32(raw + 100), &v->ext4.super_view);
    if (!ret) ret = storage_ext4_parse_inode(raw, size, &v->ext4.super_view, out);
    if (owned) kernel_free(owned);
    if (ret < 0) { v->ext4.fs_error = 1; return -RELIEFOS_EIO; }
    return out->mode ? 0 : -RELIEFOS_ENOENT;
}

int storage_ext4_read_file_range(struct storage_volume *v, uint64_t ino,
                                const struct ext4_inode_view *in, uint64_t offset,
                                void *buffer, uint32_t len, uint32_t *out_read)
{
    uint32_t done = 0;
    int ret = 0;
    if (out_read) *out_read = 0;
    if (!v || !in || (!buffer && len) ||
        (v->ext4.block_size != 1024 && v->ext4.block_size != 2048 && v->ext4.block_size != 4096))
        return -RELIEFOS_EINVAL;
    if (!len || offset >= in->size) return 0;
    if (len > in->size - offset) len = (uint32_t)(in->size - offset);
    /* Inline fast symlinks do not contain block pointers; xattrs can account
     * for i_blocks, so the size/flags contract selects their inline payload. */
    if ((in->mode & EXT2_S_IFMT) == EXT2_S_IFLNK && !(in->flags & EXT4_EXTENTS_FL) &&
        in->size < sizeof(in->i_block_raw)) {
        storage_memcpy(buffer, in->i_block_raw + offset, len);
        if (out_read) *out_read = len;
        return 0;
    }
    const uint32_t bs = v->ext4.block_size;
    while (done < len) {
        uint64_t pos = offset + done;
        uint32_t inside = pos % bs;
        struct storage_ext4_map_result map;
        ret = storage_ext4_map_block(v, ino, in, pos / bs, false, &map);
        if (ret < 0) break;
        uint64_t available = (uint64_t)map.length * bs - inside;
        uint32_t take = available < len - done ? (uint32_t)available : len - done;
        if (map.hole || map.unwritten) {
            storage_memzero((uint8_t *)buffer + done, take);
        } else if (inside || take < bs) {
            uint8_t *data;
            if (take > bs - inside) take = bs - inside;
            ret = storage_ext4_cache_get(v, map.physical, &data, false);
            if (ret < 0) break;
            storage_memcpy((uint8_t *)buffer + done, data + inside, take);
        } else {
            uint32_t blocks = take / bs;
            if (blocks > EXT4_READAHEAD_BLOCKS) blocks = EXT4_READAHEAD_BLOCKS;
            ret = storage_ext4_cache_read_blocks(v, map.physical, blocks, (uint8_t *)buffer + done);
            if (ret < 0) break;
            take = blocks * bs;
        }
        done += take;
    }
    v->ext4.read_bytes += done;
    if (out_read) *out_read = done;
    return ret;
}

static int ext4_ops_stop(struct storage_ext4_handle *h,int ret)
{
    if (ret<0) storage_ext4_journal_abort(h,ret);
    int stop=storage_ext4_journal_stop(h); return ret<0?ret:stop;
}
static void ext4_ops_touch(struct ext4_inode_view *in)
{
    struct reliefos_time_info now;
    if (!time_wall_clock(&now)) in->mtime=in->ctime=(uint32_t)now.unix_seconds;
}
/* Submit file data one block at a time through kernel memory.  The transport
 * programs the controller's PRDT with the buffer address verbatim, so the
 * caller's pointer must be a physical address: kernel_malloc() returns one
 * because the kernel is identity mapped, but `data` may be a user mapping
 * (the write(2) buffer reaches here unmodified).  Handing a user virtual
 * address to the controller silently writes the data to whatever physical
 * frame shares those low bits, which is how a copied file ends up allocated
 * but full of zeros.  Only the sub-block tail was safe before, because that
 * path already staged its bytes in a kernel scratch buffer.
 * Per-block staging keeps the buffer bounded by one block independent of `n`. */
static int ext4_ops_data_write(struct storage_volume *v,uint64_t physical,uint32_t n,const uint8_t *data)
{
    uint32_t bs=v->ext4.block_size;
    uint8_t *bounce=kernel_malloc(bs);
    if (!bounce) return -RELIEFOS_ENOMEM;
    int ret=0;
    for (uint32_t i=0;i<n && !ret;i++) {
        storage_memcpy(bounce,data+(size_t)i*bs,bs);
        ret=storage_write_device(v,v->ext_start_lba+(physical+i)*(bs/512),bs/512,bounce);
        if (!ret) storage_ext4_cache_finish(v,physical+i,bounce);
    }
    kernel_free(bounce);
    return ret;
}
int storage_ext4_write_file_range(struct storage_volume *v,uint64_t ino,uint64_t offset,
                                  const void *buffer,uint32_t len,uint32_t *out_written)
{
    if (out_written) *out_written=0;
    if (!v || (!buffer && len)) return -RELIEFOS_EINVAL;
    uint32_t bs=v->ext4.block_size,done=0; int ret=0;
    if (!bs || offset>(uint64_t)(EXT4_MAX_LOGICAL_BLOCK+1ULL)*bs ||
        len>(uint64_t)(EXT4_MAX_LOGICAL_BLOCK+1ULL)*bs-offset) return -RELIEFOS_EFBIG;
    uint8_t *scratch=kernel_malloc(bs); if (!scratch) return -RELIEFOS_ENOMEM;
    while (done<len) {
        struct storage_ext4_handle h; struct ext4_inode_view in; struct storage_ext4_map_result map;
        ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) break;
        uint64_t pos=offset+done,logical=pos/bs,physical=0;
        uint32_t inside=pos%bs,take=0,blocks=0; bool allocated=false;
        ret=storage_ext4_read_inode(v,ino,&in); if (ret<0) goto step;
        ret=storage_ext4_map_block(v,ino,&in,logical,false,&map); if (ret<0) goto step;
        blocks=(len-done)/bs;
        if (inside || !blocks) blocks=1;
        if (blocks>EXT4_READAHEAD_BLOCKS) blocks=EXT4_READAHEAD_BLOCKS;
        if (blocks>map.length) blocks=map.length;
        physical=map.physical;
        if (map.hole) {
            ret=storage_ext4_alloc_blocks(v,v->ext4.next_goal_block,blocks,&physical,&blocks);
            if (ret<0) goto step;
            allocated=true;
        }
        take=blocks*bs-inside; if (take>len-done) take=len-done;
        if (inside || take<bs) {
            if (map.hole || map.unwritten) storage_memzero(scratch,bs);
            else { ret=storage_ext4_cache_read_blocks(v,physical,1,scratch); if (ret<0) goto step; }
            /* A previous EOF's tail must never reappear across a sparse gap. */
            if (in.size/bs==logical && in.size<pos && in.size%bs)
                storage_memzero(scratch+in.size%bs,inside-in.size%bs);
            storage_memcpy(scratch+inside,(const uint8_t *)buffer+done,take);
            ret=ext4_ops_data_write(v,physical,1,scratch);
        } else ret=ext4_ops_data_write(v,physical,blocks,(const uint8_t *)buffer+done);
        if (ret<0) goto step;
        if (map.hole || map.unwritten) {
            ret=storage_ext4_change_mapping(v,ino,&in,logical,physical,blocks,false,false);
            if (ret<0) goto step;
        }
        if (pos+take>in.size) in.size=pos+take;
        ext4_ops_touch(&in);
        ret=storage_ext4_write_inode(v,ino,&in);
step:
        /* Without a journal, a failed tree insertion must at least return the
         * unattached data reservation. Journaled operations roll back as one. */
        if (ret<0 && allocated && !v->ext4.journal)
            storage_ext4_free_blocks(v,physical,blocks);
        ret=ext4_ops_stop(&h,ret); if (ret<0) break;
        done+=take;
    }
    kernel_free(scratch); if (out_written) *out_written=done; return ret;
}

/* Zero initialized partial blocks without allocating holes or converting
 * unwritten extents. Used by truncate and hole punch. */
static int ext4_ops_zero_edge(struct storage_volume *v,uint64_t ino,uint64_t start,uint64_t end)
{
    if (start>=end) return 0;
    struct ext4_inode_view in; struct storage_ext4_map_result map;
    int ret=storage_ext4_read_inode(v,ino,&in); if (ret<0) return ret;
    uint32_t bs=v->ext4.block_size;
    ret=storage_ext4_map_block(v,ino,&in,start/bs,false,&map); if (ret<0) return ret;
    if (map.hole || map.unwritten) return 0;
    uint8_t *data=kernel_malloc(bs); if (!data) return -RELIEFOS_ENOMEM;
    ret=storage_ext4_cache_read_blocks(v,map.physical,1,data);
    if (!ret) { storage_memzero(data+start%bs,end-start); ret=ext4_ops_data_write(v,map.physical,1,data); }
    kernel_free(data); return ret;
}

/* Legacy s_last_orphan/i_dtime chain, also understood by Linux. A persisted
 * new size and orphan insertion precede bounded truncate transactions. */
static int ext4_ops_orphan_head(struct storage_volume *v,uint64_t *head,bool set)
{
    uint32_t bs=v->ext4.block_size; uint8_t *data;
    int ret=storage_ext4_cache_get(v,1024/bs,&data,set); if (ret<0) return ret;
    uint8_t *sb=data+1024%bs;
    if (!set) { *head=ext4_get_le32(sb+0xe8); return 0; }
    ext4_put_le32(sb+0xe8,*head);
    ret=storage_ext4_update_super_checksum(sb,1024); if (ret<0) return ret;
    return storage_ext4_cache_mark_dirty(v,1024/bs);
}
int storage_ext4_orphan_add(struct storage_volume *v,uint64_t ino,struct ext4_inode_view *in)
{
    uint64_t head; int ret=ext4_ops_orphan_head(v,&head,false); if (ret<0) return ret;
    if (head>v->ext4.inodes_count) return -RELIEFOS_EIO;
    uint64_t next=head;
    for (uint64_t steps=0;next;steps++) {
        if (next==ino) return storage_ext4_write_inode(v,ino,in);
        if (steps>=v->ext4.inodes_count || next>v->ext4.inodes_count) return -RELIEFOS_EIO;
        struct ext4_inode_view item; ret=storage_ext4_read_inode(v,next,&item); if (ret<0) return ret;
        next=item.dtime;
    }
    in->dtime=head; head=ino;
    ret=storage_ext4_write_inode(v,ino,in); if (ret<0) return ret;
    return ext4_ops_orphan_head(v,&head,true);
}
static int ext4_ops_orphan_del(struct storage_volume *v,uint64_t ino,struct ext4_inode_view *in)
{
    uint64_t head; int ret=ext4_ops_orphan_head(v,&head,false); if (ret<0) return ret;
    if (head==ino) { head=in->dtime; ret=ext4_ops_orphan_head(v,&head,true); }
    else {
        uint64_t current=head; bool found=false;
        for (uint64_t steps=0;current && steps<v->ext4.inodes_count;steps++) {
            struct ext4_inode_view previous; ret=storage_ext4_read_inode(v,current,&previous); if (ret<0) return ret;
            if (previous.dtime==ino) { previous.dtime=in->dtime;
                ret=storage_ext4_write_inode(v,current,&previous); found=true; break; }
            current=previous.dtime;
        }
        if (!found) return -RELIEFOS_EIO;
    }
    if (ret<0) return ret;
    in->dtime=0; return storage_ext4_write_inode(v,ino,in);
}
static int ext4_ops_truncate(struct storage_volume *v,uint64_t ino,uint64_t size,bool destroy)
{
    if (!v || !v->ext4.block_size || size>(EXT4_MAX_LOGICAL_BLOCK+1ULL)*v->ext4.block_size)
        return -RELIEFOS_EFBIG;
    struct ext4_inode_view in; struct storage_ext4_handle h;
    int ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
    ret=storage_ext4_read_inode(v,ino,&in); if (ret<0) return ext4_ops_stop(&h,ret);
    uint32_t bs=v->ext4.block_size;
    bool no_data=((in.mode&EXT2_S_IFMT)==EXT2_S_IFLNK && !(in.flags&EXT4_EXTENTS_FL) && in.size<60) ||
                 ((in.mode&EXT2_S_IFMT)!=EXT2_S_IFLNK && (in.mode&EXT2_S_IFMT)!=EXT2_S_IFREG &&
                  (in.mode&EXT2_S_IFMT)!=EXT2_S_IFDIR);
    if (destroy && in.links_count) return ext4_ops_stop(&h,-RELIEFOS_EINVAL);
    if (size>in.size) {
        if (in.size%bs) ret=ext4_ops_zero_edge(v,ino,in.size,(in.size/bs+1)*bs);
        if (!ret) { in.size=size; ext4_ops_touch(&in); ret=storage_ext4_write_inode(v,ino,&in); }
        return ext4_ops_stop(&h,ret);
    }
    if (!no_data && size%bs) ret=ext4_ops_zero_edge(v,ino,size,(size/bs+1)*bs);
    if (no_data && !size) storage_memzero(in.i_block_raw,60);
    if (!ret) { in.size=size; ext4_ops_touch(&in); ret=storage_ext4_orphan_add(v,ino,&in); }
    ret=ext4_ops_stop(&h,ret); if (ret<0) return ret;
    uint64_t end=EXT4_MAX_LOGICAL_BLOCK+1ULL;
    if (!(in.flags&EXT4_EXTENTS_FL)) {
        uint64_t n=bs/4,limit=12+n+n*n+n*n*n; if (end>limit) end=limit;
    }
    if (!no_data) { ret=storage_ext4_remove_range(v,ino,(size+bs-1)/bs,end); if (ret<0) return ret; }
    ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
    ret=storage_ext4_read_inode(v,ino,&in);
    if (!ret && destroy) ret=storage_ext4_drop_xattrs(v,ino,&in);
    if (!ret && (in.links_count || destroy)) ret=ext4_ops_orphan_del(v,ino,&in);
    /* The orphan removal and inode-bitmap release must share a transaction:
     * a crash between them must not strand an unreachable allocated inode. */
    if (!ret && destroy) ret=storage_ext4_free_inode(v,ino,(in.mode&EXT2_S_IFMT)==EXT2_S_IFDIR);
    return ext4_ops_stop(&h,ret);
}
int storage_ext4_truncate(struct storage_volume *v,uint64_t ino,uint64_t size)
{ return ext4_ops_truncate(v,ino,size,false); }
int storage_ext4_destroy_inode(struct storage_volume *v,uint64_t ino)
{ return ext4_ops_truncate(v,ino,0,true); }
int storage_ext4_recover_orphans(struct storage_volume *v)
{
    for (uint64_t i=0;i<v->ext4.inodes_count;i++) {
        uint64_t head; int ret=ext4_ops_orphan_head(v,&head,false); if (ret<0 || !head) return ret;
        if (head>v->ext4.inodes_count) return -RELIEFOS_EIO;
        struct ext4_inode_view in; ret=storage_ext4_read_inode(v,head,&in); if (ret<0) return ret;
        if (in.dtime==head) return -RELIEFOS_EIO;
        ret=ext4_ops_truncate(v,head,in.links_count?in.size:0,!in.links_count); if (ret<0) return ret;
    }
    return -RELIEFOS_EIO;
}
int storage_ext4_fallocate(struct storage_volume *v,uint64_t ino,uint32_t mode,uint64_t offset,uint64_t len)
{
    if (mode&~(1u|2u|16u)) return -RELIEFOS_EOPNOTSUPP;
    if ((mode&2) && (!(mode&1) || (mode&16))) return -RELIEFOS_EOPNOTSUPP;
    if (!v || !len || offset>UINT64_MAX-len) return -RELIEFOS_EINVAL;
    uint32_t bs=v->ext4.block_size;
    if (!bs || offset+len>(EXT4_MAX_LOGICAL_BLOCK+1ULL)*bs) return -RELIEFOS_EFBIG;
    if (v->read_only_reason || v->ext4.fs_error) return -RELIEFOS_EROFS;
    uint64_t end=offset+len;
    if (mode&2) {
        uint64_t first=(offset+bs-1)/bs,last=end/bs;
        int ret=ext4_ops_zero_edge(v,ino,offset,end<(offset/bs+1)*bs?end:(offset/bs+1)*bs);
        if (ret<0) return ret;
        if (end%bs && end/bs!=offset/bs) { ret=ext4_ops_zero_edge(v,ino,end/bs*bs,end); if (ret<0) return ret; }
        return first<last ? storage_ext4_remove_range(v,ino,first,last) : 0;
    }
    uint8_t *zeros=kernel_malloc(bs*EXT4_READAHEAD_BLOCKS); if (!zeros) return -RELIEFOS_ENOMEM;
    storage_memzero(zeros,bs*EXT4_READAHEAD_BLOCKS); int ret=0;
    for (uint64_t at=offset/bs;at<(end+bs-1)/bs;) {
        struct storage_ext4_handle h; struct ext4_inode_view in; struct storage_ext4_map_result map;
        ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) break;
        ret=storage_ext4_read_inode(v,ino,&in); if (ret<0) { ext4_ops_stop(&h,ret); break; }
        ret=storage_ext4_map_block(v,ino,&in,at,false,&map); uint32_t n=0;
        if (!ret) {
            n=(end+bs-1)/bs-at; if (n>EXT4_READAHEAD_BLOCKS) n=EXT4_READAHEAD_BLOCKS;
            if (n>map.length) n=map.length;
            bool extents=(in.flags&EXT4_EXTENTS_FL)!=0;
            if (map.hole) {
                uint64_t b; ret=storage_ext4_alloc_blocks(v,v->ext4.next_goal_block,n,&b,&n);
                if (!ret && !extents) ret=ext4_ops_data_write(v,b,n,zeros);
                if (!ret) ret=storage_ext4_change_mapping(v,ino,&in,at,b,n,extents,false);
            }
            if (!ret && (mode&16)) {
                /* Zero full initialized ranges; edges preserve outside bytes. */
                if (!map.hole && !map.unwritten) {
                    for (uint32_t j=0;j<n && !ret;j++) {
                        uint64_t a=(at+j)*bs,z=a+bs;
                        if (a<offset) a=offset; if (z>end) z=end;
                        ret=ext4_ops_zero_edge(v,ino,a,z);
                    }
                }
            }
            if (!ret && !(mode&1) && in.size<end) { in.size=end; ret=storage_ext4_write_inode(v,ino,&in); }
        }
        ret=ext4_ops_stop(&h,ret); if (ret<0) break; at+=n;
    }
    kernel_free(zeros); return ret;
}

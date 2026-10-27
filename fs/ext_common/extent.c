/* Bounded, non-recursive extent/indirect lookup. Callers serialize a volume
 * through the storage execution lock; no pointer survives a cache lookup.
 * Linux fs/ext4/extents.c and ext4_extents.h define the disk format. */
#include "../../block/core/storage_internal.h"

static int ext4_map_bad(struct storage_volume *volume)
{
    volume->ext4.fs_error = 1;
    return -RELIEFOS_EIO;
}

static bool ext4_map_block_valid(const struct storage_volume *v, uint64_t block, uint32_t n)
{
    return n && block > v->ext4.first_data_block && block < v->ext4.blocks_count &&
           n <= v->ext4.blocks_count - block;
}

static uint32_t ext4_map_run(uint64_t n)
{ return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n; }

static int ext4_extent_lookup(struct storage_volume *v, uint64_t ino,
                              const struct ext4_inode_view *in, uint64_t logical,
                              struct storage_ext4_map_result *out)
{
    const uint8_t *node = in->i_block_raw;
    uint64_t visited[EXT4_MAX_EXTENT_DEPTH] = {0};
    uint64_t lower = 0, upper = (uint64_t)EXT4_MAX_LOGICAL_BLOCK + 1;
    uint32_t length = 60, expected_depth = ext4_get_le16(node + 6), level = 0;
    if (expected_depth > EXT4_MAX_EXTENT_DEPTH) return ext4_map_bad(v);
    for (;;) {
        struct storage_ext4_extent_header_view h;
        if (storage_ext4_parse_extent_header(node, length, &h) < 0 ||
            !h.max_entries || h.max_entries > (length - 12) / 12 ||
            h.depth != expected_depth || (!h.entries && h.depth))
            return ext4_map_bad(v);
        if (level && (v->ext4.super_view.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) {
            uint32_t tail = 12 + 12 * h.max_entries;
            uint8_t word[4];
            if (tail + 4 > length) return ext4_map_bad(v);
            uint32_t crc = storage_ext4_super_csum_seed(&v->ext4.super_view);
            ext4_put_le32(word, (uint32_t)ino); crc = storage_ext4_crc32c(crc, word, 4);
            ext4_put_le32(word, in->generation); crc = storage_ext4_crc32c(crc, word, 4);
            if (storage_ext4_crc32c(crc, node, tail) != ext4_get_le32(node + tail))
                return ext4_map_bad(v);
        }
        uint64_t previous_end = lower;
        for (uint32_t i = 0; i < h.entries; ++i) {
            const uint8_t *e = node + 12 + i * 12;
            uint64_t start = ext4_get_le32(e), end, physical;
            uint32_t n;
            if (h.depth) {
                n = 1; end = start + 1;
                physical = ext4_get_le32(e + 4) | ((uint64_t)ext4_get_le16(e + 8) << 32);
            } else {
                n = ext4_get_le16(e + 4);
                if (n > 32768) n -= 32768;
                end = start + n;
                physical = ext4_get_le32(e + 8) | ((uint64_t)ext4_get_le16(e + 6) << 32);
            }
            if ((level && !i && start != lower) || start < previous_end || end > upper ||
                !ext4_map_block_valid(v, physical, n)) return ext4_map_bad(v);
            for (uint32_t j = 0; j < level; ++j)
                if (visited[j] >= physical && visited[j] - physical < n)
                    return ext4_map_bad(v);
            previous_end = end;
        }
        /* Binary upper_bound: select the final key <= logical. */
        uint32_t lo = 0, hi = h.entries;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (ext4_get_le32(node + 12 + mid * 12) <= logical) lo = mid + 1;
            else hi = mid;
        }
        if (!h.depth) {
            if (lo) {
                const uint8_t *e = node + 12 + (lo - 1) * 12;
                uint64_t start = ext4_get_le32(e);
                uint32_t raw_len = ext4_get_le16(e + 4);
                uint32_t n = raw_len > 32768 ? raw_len - 32768 : raw_len;
                if (logical - start < n) {
                    out->physical = (ext4_get_le32(e + 8) |
                                     ((uint64_t)ext4_get_le16(e + 6) << 32)) + logical - start;
                    out->length = n - (uint32_t)(logical - start);
                    out->unwritten = raw_len > 32768;
                    return 0;
                }
            }
            if (lo < h.entries) upper = ext4_get_le32(node + 12 + lo * 12);
            out->hole = true; out->length = ext4_map_run(upper - logical);
            return out->length ? 0 : ext4_map_bad(v);
        }
        if (!lo) {
            out->hole = true;
            out->length = ext4_map_run(ext4_get_le32(node + 12) - logical);
            return out->length ? 0 : ext4_map_bad(v);
        }
        const uint8_t *selected = node + 12 + (lo - 1) * 12;
        lower = ext4_get_le32(selected);
        if (lo < h.entries) upper = ext4_get_le32(selected + 12);
        uint64_t child = ext4_get_le32(selected + 4) |
                         ((uint64_t)ext4_get_le16(selected + 8) << 32);
        uint8_t *data;
        visited[level++] = child;
        int ret = storage_ext4_cache_get(v, child, &data, false);
        if (ret < 0) return ret;
        node = data; length = v->ext4.block_size; expected_depth--;
    }
}

static int ext4_indirect_lookup(struct storage_volume *v, const struct ext4_inode_view *in,
                                uint64_t logical, struct storage_ext4_map_result *out)
{
    uint64_t n = v->ext4.block_size / 4, offset = logical, span = 1;
    uint64_t path[3] = {0}; uint32_t levels = 0;
    const uint8_t *entries = in->i_block_raw;
    uint32_t index = (uint32_t)logical, count = 12;
    if (logical >= 12) {
        offset -= 12;
        for (levels = 1; levels <= 3; ++levels) {
            span *= n;
            if (offset < span) break;
            offset -= span;
        }
        if (levels > 3) return -RELIEFOS_EFBIG;
        uint64_t ptr = ext4_get_le32(in->i_block_raw + (11 + levels) * 4);
        for (uint32_t depth = 0; depth < levels; ++depth) {
            if (!ptr) {
                out->hole = true; out->length = ext4_map_run(span - offset);
                return 0;
            }
            if (!ext4_map_block_valid(v, ptr, 1)) return ext4_map_bad(v);
            for (uint32_t i = 0; i < depth; ++i)
                if (path[i] == ptr) return ext4_map_bad(v);
            path[depth] = ptr;
            uint8_t *data;
            int ret = storage_ext4_cache_get(v, ptr, &data, false);
            if (ret < 0) return ret;
            span /= n; index = (uint32_t)(offset / span); offset %= span;
            entries = data; count = (uint32_t)n;
            ptr = ext4_get_le32(entries + index * 4);
        }
    }
    uint64_t physical = ext4_get_le32(entries + index * 4);
    if (physical && !ext4_map_block_valid(v, physical, 1)) return ext4_map_bad(v);
    for (uint32_t i = 0; i < levels; ++i)
        if (physical && physical == path[i]) return ext4_map_bad(v);
    out->physical = physical; out->hole = !physical; out->length = 1;
    for (uint32_t i = index + 1; i < count; ++i) {
        uint64_t next = ext4_get_le32(entries + i * 4);
        if (physical ? next != physical + out->length : next != 0) break;
        if (next && !ext4_map_block_valid(v, next, 1)) return ext4_map_bad(v);
        for (uint32_t j = 0; j < levels; ++j)
            if (next && next == path[j]) return ext4_map_bad(v);
        ++out->length;
    }
    return 0;
}

int storage_ext4_map_block(struct storage_volume *v, uint64_t ino,
                           const struct ext4_inode_view *in, uint64_t logical,
                           bool allocate, struct storage_ext4_map_result *out)
{
    if (!v || !in || !out || !ino || ino > v->ext4.inodes_count ||
        (v->ext4.block_size != 1024 && v->ext4.block_size != 2048 && v->ext4.block_size != 4096))
        return -RELIEFOS_EINVAL;
    *out = (struct storage_ext4_map_result){0};
    if (logical > EXT4_MAX_LOGICAL_BLOCK) return -RELIEFOS_EFBIG;
    if (allocate) {
        int ret=storage_ext4_map_block(v,ino,in,logical,false,out);
        if (ret<0 || !out->hole) return ret;
        /* Allocation exposes zeros and preserves EOF; callers subsequently
         * write the block and size in their enclosing transaction. */
        ret=storage_ext4_fallocate(v,ino,1,logical*v->ext4.block_size,v->ext4.block_size);
        if (ret<0) return ret;
        struct ext4_inode_view updated;
        ret=storage_ext4_read_inode(v,ino,&updated);
        return ret<0?ret:storage_ext4_map_block(v,ino,&updated,logical,false,out);
    }
    if (in->flags & EXT4_EXTENTS_FL) {
        if (!(v->ext4.super_view.feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS))
            return ext4_map_bad(v);
        return ext4_extent_lookup(v, ino, in, logical, out);
    }
    return ext4_indirect_lookup(v, in, logical, out);
}

/* A mutation retains only one root-to-leaf path. Two spare records per node
 * cover an unwritten split; memory and journal credits do not scale with the
 * file size. All metadata writes go through the transaction-aware cache. */
struct ext4_edit_path { uint8_t *node; uint64_t block; uint16_t slot; };
static void ext4_edit_header(uint8_t *p,uint16_t count,uint16_t max,uint16_t depth)
{
    ext4_put_le16(p,0xf30a); ext4_put_le16(p+2,count);
    ext4_put_le16(p+4,max); ext4_put_le16(p+6,depth); ext4_put_le32(p+8,0);
}
static uint64_t ext4_edit_phys(const uint8_t *e)
{ return ext4_get_le32(e+8)|((uint64_t)ext4_get_le16(e+6)<<32); }
static uint32_t ext4_edit_len(const uint8_t *e)
{ uint32_t n=ext4_get_le16(e+4); return n>32768 ? n-32768 : n; }
static void ext4_edit_extent(uint8_t *p,uint64_t l,uint64_t b,uint32_t n,bool unwritten)
{
    ext4_put_le32(p,l); ext4_put_le16(p+4,n+(unwritten?32768:0));
    ext4_put_le16(p+6,b>>32); ext4_put_le32(p+8,b);
}
static void ext4_edit_index(uint8_t *p,uint32_t l,uint64_t b)
{ ext4_put_le32(p,l); ext4_put_le32(p+4,b); ext4_put_le16(p+8,b>>32); ext4_put_le16(p+10,0); }
static void ext4_edit_splice(uint8_t *p,unsigned slot,unsigned remove,const uint8_t *add,unsigned nr)
{
    unsigned count=ext4_get_le16(p+2);
    if (nr>remove) for (unsigned i=count;i>slot+remove;i--)
        storage_memcpy(p+12+(i-1+nr-remove)*12,p+12+(i-1)*12,12);
    else for (unsigned i=slot+remove;i<count;i++)
        storage_memcpy(p+12+(i+nr-remove)*12,p+12+i*12,12);
    for (unsigned i=0;i<nr;i++) storage_memcpy(p+12+(slot+i)*12,add+i*12,12);
    ext4_put_le16(p+2,count+nr-remove);
}
static int ext4_edit_store(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in,
                            uint64_t block,uint8_t *raw)
{
    uint32_t bs=v->ext4.block_size,tail=12+12*ext4_get_le16(raw+4);
    if (v->ext4.super_view.feature_ro_compat&EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        uint8_t word[4]; uint32_t crc=storage_ext4_super_csum_seed(&v->ext4.super_view);
        ext4_put_le32(word,ino); crc=storage_ext4_crc32c(crc,word,4);
        ext4_put_le32(word,in->generation); crc=storage_ext4_crc32c(crc,word,4);
        ext4_put_le32(raw+tail,storage_ext4_crc32c(crc,raw,tail));
    }
    uint8_t *data; int ret=storage_ext4_cache_get(v,block,&data,true); if (ret<0) return ret;
    storage_memcpy(data,raw,bs); return storage_ext4_cache_mark_dirty(v,block);
}
static int ext4_edit_alloc(struct storage_volume *v,struct ext4_inode_view *in,uint64_t *block)
{
    uint32_t got; int ret=storage_ext4_alloc_blocks(v,v->ext4.next_goal_block,1,block,&got);
    if (!ret) in->blocks+=v->ext4.block_size/512;
    return ret;
}
static int ext4_edit_free(struct storage_volume *v,struct ext4_inode_view *in,uint64_t b,uint32_t n)
{
    uint64_t sectors=(uint64_t)n*(v->ext4.block_size/512);
    if (in->blocks<sectors) return ext4_map_bad(v);
    int ret=storage_ext4_free_blocks(v,b,n); if (!ret) in->blocks-=sectors;
    return ret;
}
static int ext4_edit_indirect(struct storage_volume *v,struct ext4_inode_view *in,
                              uint64_t logical,uint64_t physical,bool release)
{
    uint32_t n=v->ext4.block_size/4,level=0,slot[4];
    uint64_t at=logical,span=1,blocks[4]={0};
    if (physical>UINT32_MAX) return -RELIEFOS_EFBIG;
    if (at<12) slot[0]=at;
    else {
        at-=12;
        for (level=1;level<=3;level++) { span*=n; if (at<span) break; at-=span; }
        if (level>3) return -RELIEFOS_EFBIG;
        slot[0]=11+level;
        for (uint32_t i=1;i<=level;i++) { span/=n; slot[i]=at/span; at%=span; }
    }
    uint8_t *data=in->i_block_raw; int ret=0;
    for (uint32_t i=0;i<level;i++) {
        uint64_t b=ext4_get_le32(data+slot[i]*4);
        if (!b) {
            if (!physical) return 0;
            ret=ext4_edit_alloc(v,in,&b); if (ret<0) return ret;
            if (b>UINT32_MAX) { ext4_edit_free(v,in,b,1); return -RELIEFOS_EFBIG; }
            if (i) { ret=storage_ext4_cache_get(v,blocks[i],&data,true); if (ret<0) return ret; }
            ext4_put_le32(data+slot[i]*4,b);
            if (i) { ret=storage_ext4_cache_mark_dirty(v,blocks[i]); if (ret<0) return ret; }
            ret=storage_ext4_cache_get(v,b,&data,true); if (ret<0) return ret;
            storage_memzero(data,v->ext4.block_size);
            ret=storage_ext4_cache_mark_dirty(v,b); if (ret<0) return ret;
        }
        blocks[i+1]=b;
        ret=storage_ext4_cache_get(v,b,&data,false); if (ret<0) return ret;
    }
    uint64_t old=ext4_get_le32(data+slot[level]*4);
    if (release && old && old!=physical) { ret=ext4_edit_free(v,in,old,1); if (ret<0) return ret; }
    if (!old && physical) in->blocks+=v->ext4.block_size/512;
    if (level) { ret=storage_ext4_cache_get(v,blocks[level],&data,true); if (ret<0) return ret; }
    ext4_put_le32(data+slot[level]*4,physical);
    if (level) { ret=storage_ext4_cache_mark_dirty(v,blocks[level]); if (ret<0) return ret; }
    /* Reclaim an empty indirect block and its ancestors. */
    for (uint32_t i=level;!physical && i;i--) {
        ret=storage_ext4_cache_get(v,blocks[i],&data,false); if (ret<0) return ret;
        bool empty=true; for (uint32_t j=0;j<n;j++) if (ext4_get_le32(data+j*4)) { empty=false; break; }
        if (!empty) break;
        ret=ext4_edit_free(v,in,blocks[i],1); if (ret<0) return ret;
        data=in->i_block_raw;
        if (i>1) { ret=storage_ext4_cache_get(v,blocks[i-1],&data,true); if (ret<0) return ret; }
        ext4_put_le32(data+slot[i-1]*4,0);
        if (i>1) { ret=storage_ext4_cache_mark_dirty(v,blocks[i-1]); if (ret<0) return ret; }
    }
    return 0;
}

int storage_ext4_change_mapping(struct storage_volume *v,uint64_t ino,struct ext4_inode_view *in,
                                uint64_t logical,uint64_t physical,uint32_t len,bool unwritten,bool release)
{
    if (!len || logical>EXT4_MAX_LOGICAL_BLOCK || len>(uint64_t)EXT4_MAX_LOGICAL_BLOCK+1-logical ||
        (physical && (!ext4_map_block_valid(v,physical,len) || len>(unwritten?32767u:32768u))))
        return -RELIEFOS_EINVAL;
    struct storage_ext4_map_result map;
    int ret=storage_ext4_map_block(v,ino,in,logical,false,&map); if (ret<0) return ret;
    if (len>map.length) return -RELIEFOS_EINVAL;
    if (!(in->flags&EXT4_EXTENTS_FL)) {
        if (unwritten) return -RELIEFOS_EOPNOTSUPP;
        for (uint32_t i=0;i<len;i++) {
            ret=ext4_edit_indirect(v,in,logical+i,physical?physical+i:0,release); if (ret<0) return ret;
        }
        return storage_ext4_write_inode(v,ino,in);
    }
    unsigned depth=ext4_get_le16(in->i_block_raw+6),stride=v->ext4.block_size+36;
    struct ext4_edit_path path[EXT4_MAX_EXTENT_DEPTH+1];
    uint8_t *memory=kernel_malloc((depth+2)*stride);
    if (!memory) return -RELIEFOS_ENOMEM;
    storage_memzero(memory,(depth+2)*stride);
    for (unsigned i=0;i<=depth;i++) path[i]=(struct ext4_edit_path){.node=memory+i*stride};
    storage_memcpy(path[0].node,in->i_block_raw,60);
    for (unsigned i=0;i<depth;i++) {
        uint8_t *p=path[i].node; unsigned count=ext4_get_le16(p+2),slot=0;
        if (!count) { ret=ext4_map_bad(v); goto out; }
        while (slot+1<count && ext4_get_le32(p+24+slot*12)<=logical) slot++;
        path[i].slot=slot; const uint8_t *e=p+12+slot*12;
        uint64_t b=ext4_get_le32(e+4)|((uint64_t)ext4_get_le16(e+8)<<32);
        /* Lookup the selected child's first key as well when inserting before
         * the first key (the ordinary gap lookup deliberately skips it). */
        struct storage_ext4_map_result valid;
        ret=ext4_extent_lookup(v,ino,in,ext4_get_le32(e),&valid); if (ret<0) goto out;
        uint8_t *data; ret=storage_ext4_cache_get(v,b,&data,false); if (ret<0) goto out;
        storage_memcpy(path[i+1].node,data,v->ext4.block_size); path[i+1].block=b;
    }
    uint8_t *leaf=path[depth].node,add[36]; unsigned count=ext4_get_le16(leaf+2),slot=0,nr=0,remove=0;
    while (slot<count && (uint64_t)ext4_get_le32(leaf+12+slot*12)+ext4_edit_len(leaf+12+slot*12)<=logical) slot++;
    if (slot<count && ext4_get_le32(leaf+12+slot*12)<=logical) {
        uint8_t *e=leaf+12+slot*12; uint64_t l=ext4_get_le32(e),b=ext4_edit_phys(e);
        uint32_t n=ext4_edit_len(e); bool u=ext4_get_le16(e+4)>32768; remove=1;
        if (logical>l) ext4_edit_extent(add+12*nr++,l,b,logical-l,u);
        if (physical) ext4_edit_extent(add+12*nr++,logical,physical,len,unwritten);
        if (logical+len<l+n) ext4_edit_extent(add+12*nr++,logical+len,b+logical+len-l,l+n-logical-len,u);
        if (release) { ret=ext4_edit_free(v,in,b+logical-l,len); if (ret<0) goto out; }
    } else if (physical) ext4_edit_extent(add+12*nr++,logical,physical,len,unwritten);
    if (physical && (map.hole || release)) in->blocks+=(uint64_t)len*(v->ext4.block_size/512);
    ext4_edit_splice(leaf,slot,remove,add,nr);
    /* Merge neighbors after insertion or unwritten conversion. */
    for (unsigned i=1;i<ext4_get_le16(leaf+2);) {
        uint8_t *a=leaf+12+(i-1)*12,*b=a+12;
        uint32_t an=ext4_edit_len(a),bn=ext4_edit_len(b); bool u=ext4_get_le16(a+4)>32768;
        if ((uint64_t)ext4_get_le32(a)+an==ext4_get_le32(b) && ext4_edit_phys(a)+an==ext4_edit_phys(b) &&
            u==(ext4_get_le16(b+4)>32768) && an+bn<=(u?32767u:32768u)) {
            ext4_put_le16(a+4,an+bn+(u?32768:0)); ext4_edit_splice(leaf,i,1,NULL,0);
        } else i++;
    }
    uint8_t *scratch=memory+(depth+1)*stride;
    for (int i=(int)depth;i>=0;i--) {
        uint8_t *p=path[i].node; unsigned n=ext4_get_le16(p+2),max=ext4_get_le16(p+4),d=ext4_get_le16(p+6);
        if (!i) {
            if (!n) { storage_memzero(p,60); ext4_edit_header(p,0,4,0); }
            else if (n>4) {
                if (d==EXT4_MAX_EXTENT_DEPTH) { ret=-RELIEFOS_EFBIG; goto out; }
                uint64_t b; ret=ext4_edit_alloc(v,in,&b); if (ret<0) goto out;
                ext4_put_le16(p+4,(v->ext4.block_size-12)/12);
                ret=ext4_edit_store(v,ino,in,b,p); if (ret<0) goto out;
                uint32_t key=ext4_get_le32(p+12); storage_memzero(p,60);
                ext4_edit_header(p,1,4,d+1); ext4_edit_index(p+12,key,b);
            }
            storage_memcpy(in->i_block_raw,p,60); break;
        }
        uint8_t *parent=path[i-1].node; unsigned at=path[i-1].slot;
        if (!n) {
            ret=ext4_edit_free(v,in,path[i].block,1); if (ret<0) goto out;
            ext4_edit_splice(parent,at,1,NULL,0); continue;
        }
        ext4_put_le32(parent+12+at*12,ext4_get_le32(p+12));
        if (n>max) {
            uint64_t b; ret=ext4_edit_alloc(v,in,&b); if (ret<0) goto out;
            unsigned left=n/2,right=n-left; storage_memzero(scratch,stride);
            ext4_edit_header(scratch,right,max,d);
            storage_memcpy(scratch+12,p+12+left*12,right*12); ext4_put_le16(p+2,left);
            ret=ext4_edit_store(v,ino,in,b,scratch); if (ret<0) goto out;
            ext4_edit_index(add,ext4_get_le32(scratch+12),b); ext4_edit_splice(parent,at+1,0,add,1);
        }
        ret=ext4_edit_store(v,ino,in,path[i].block,p); if (ret<0) goto out;
    }
    /* Collapse a single, sufficiently small child back into the inline root. */
    while (ext4_get_le16(in->i_block_raw+6) && ext4_get_le16(in->i_block_raw+2)==1) {
        uint8_t *root=in->i_block_raw; uint64_t b=ext4_get_le32(root+16)|((uint64_t)ext4_get_le16(root+20)<<32);
        uint8_t *data; ret=storage_ext4_cache_get(v,b,&data,false); if (ret<0) goto out;
        if (ext4_get_le16(data+2)>4) break;
        storage_memcpy(scratch,data,60);
        ret=ext4_edit_free(v,in,b,1); if (ret<0) goto out;
        storage_memcpy(root,scratch,60); ext4_put_le16(root+4,4);
    }
    ret=storage_ext4_write_inode(v,ino,in);
out:
    kernel_free(memory); return ret;
}

int storage_ext4_insert_extent(struct storage_volume *v,uint64_t ino,uint64_t logical,
                               uint64_t physical,uint32_t len,bool unwritten)
{
    struct storage_ext4_handle h; struct ext4_inode_view in;
    int ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
    ret=storage_ext4_read_inode(v,ino,&in);
    if (!ret) ret=storage_ext4_change_mapping(v,ino,&in,logical,physical,len,unwritten,false);
    if (ret<0) storage_ext4_journal_abort(&h,ret);
    int stop=storage_ext4_journal_stop(&h); return ret<0?ret:stop;
}
int storage_ext4_remove_range(struct storage_volume *v,uint64_t ino,uint64_t start,uint64_t end)
{
    if (end>(uint64_t)EXT4_MAX_LOGICAL_BLOCK+1 || start>end) return -RELIEFOS_EINVAL;
    while (start<end) {
        struct storage_ext4_handle h; struct ext4_inode_view in; struct storage_ext4_map_result map;
        int ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
        ret=storage_ext4_read_inode(v,ino,&in);
        if (!ret) ret=storage_ext4_map_block(v,ino,&in,start,false,&map);
        uint32_t n=0;
        if (!ret) {
            n=map.length; if (n>end-start) n=end-start;
            /* Bound bitmap and pointer blocks touched by one transaction. */
            if (!map.hole && n>32) n=32;
            if (!map.hole) ret=storage_ext4_change_mapping(v,ino,&in,start,0,n,false,true);
        }
        if (ret<0) storage_ext4_journal_abort(&h,ret);
        int stop=storage_ext4_journal_stop(&h); if (ret<0 || stop<0) return ret<0?ret:stop;
        start+=n;
    }
    return 0;
}

int storage_ext4_fiemap(struct storage_volume *v, uint64_t ino, uint64_t start, uint64_t len,
                        struct storage_ext4_fiemap_extent *out, uint32_t capacity, uint32_t *count)
{
    struct ext4_inode_view in;
    if (!v || !count || (capacity && !out)) return -RELIEFOS_EINVAL;
    *count = 0;
    int ret = storage_ext4_read_inode(v, ino, &in);
    if (ret < 0) return ret;
    uint64_t limit=EXT4_MAX_LOGICAL_BLOCK+1ULL;
    if (!(in.flags&EXT4_EXTENTS_FL)) {
        uint64_t n=v->ext4.block_size/4,indirect=12+n+n*n+n*n*n;
        if (limit>indirect) limit=indirect;
    }
    limit*=v->ext4.block_size;
    if (!len || start >= limit) return 0;
    uint64_t end = len > limit - start ? limit : start + len;
    uint64_t pos = start, bs = v->ext4.block_size;
    while (pos < end) {
        struct storage_ext4_map_result map;
        ret = storage_ext4_map_block(v, ino, &in, pos / bs, false, &map);
        if (ret < 0) return ret;
        uint64_t bytes = (uint64_t)map.length * bs - pos % bs;
        if (bytes > end - pos) bytes = end - pos;
        if (!map.hole) {
            if (capacity && *count == capacity) return 0;
            if (out && capacity) out[*count] = (struct storage_ext4_fiemap_extent){
                .logical = pos, .physical = map.physical * bs + pos % bs,
                .length = bytes, .flags = map.unwritten ? EXT4_FIEMAP_UNWRITTEN : 0};
            if (*count == UINT32_MAX) return -RELIEFOS_EOVERFLOW;
            ++*count;
        }
        pos += bytes;
    }
    if (out && capacity && *count && end>=in.size) {
        bool last=true;
        for (uint64_t l=(end+bs-1)/bs;l<limit/bs;) {
            struct storage_ext4_map_result m;
            ret=storage_ext4_map_block(v,ino,&in,l,false,&m); if (ret<0) return ret;
            if (!m.hole) { last=false; break; } l+=m.length;
        }
        if (last) out[*count-1].flags|=EXT4_FIEMAP_LAST;
    }
    return 0;
}

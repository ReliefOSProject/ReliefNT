/* Native JBD2 ordered metadata transactions. Disk fields are big endian.
 * Fixed memory: 32 before/after block images per mounted journal plus a
 * bounded revoke table. Recovery streams the ring three times; overflow of
 * the revoke table uses a bounded-memory rescan instead of dropping revokes.
 * All entry points require the storage execution lock (never a device lock).
 * Sources: Linux fs/jbd2/{journal,commit,recovery}.c, include/linux/jbd2.h. */
#include "../block/storage_internal.h"
#define EJ_MAGIC 0xc03b3998u
#define EJ_CSUM2 8u
#define EJ_CSUM3 16u
#define EJ_REVOKES 512u

struct ext4_journal_image {
    uint64_t block;
    bool dirty;
    uint8_t before[EXT4_MAX_BLOCK_SIZE], after[EXT4_MAX_BLOCK_SIZE];
};
struct ext4_journal_revoke { uint64_t block; uint32_t sequence; bool valid; };
struct ext4_journal_scan { uint8_t descriptor[EXT4_MAX_BLOCK_SIZE], data[EXT4_MAX_BLOCK_SIZE]; };
struct storage_ext4_journal {
    struct ext4_inode_view inode;
    uint32_t first, maxlen, sequence, start, head, compat, incompat, seed;
    uint32_t count, credits, active;
    int error;
    bool committed, revoke_overflow;
    uint8_t uuid[16];
    struct storage_ext4_super_view before_super;
    uint64_t before_goal, before_window;
    struct ext4_journal_image images[EXT4_JOURNAL_CREDITS];
    struct ext4_journal_revoke revokes[EJ_REVOKES];
    struct ext4_journal_scan scan, fallback;
};
static uint32_t ej_be32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t ej_be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static void ej_put32(uint8_t *p, uint32_t n)
{ p[0]=n>>24; p[1]=n>>16; p[2]=n>>8; p[3]=n; }
static void ej_put16(uint8_t *p, uint16_t n) { p[0]=n>>8; p[1]=n; }
static bool ej_equal(const uint8_t *a, const uint8_t *b, uint32_t n)
{ for (uint32_t i=0;i<n;i++) if (a[i]!=b[i]) return false; return true; }
static bool ej_csum(const struct storage_ext4_journal *j) { return (j->incompat & 24) != 0; }
static uint32_t ej_next(const struct storage_ext4_journal *j,uint32_t n)
{ return n+1 == j->maxlen ? j->first : n+1; }
static uint32_t ej_tag_size(const struct storage_ext4_journal *j)
{ return j->incompat & EJ_CSUM3 ? 16 : 8 + ((j->incompat & 2) ? 4 : 0) + ((j->incompat & EJ_CSUM2) ? 2 : 0); }
static uint32_t ej_crc32_be(uint32_t crc,const uint8_t *p,uint32_t n)
{
    for (uint32_t i=0;i<n;i++) {
        crc ^= (uint32_t)p[i]<<24;
        for (uint32_t k=0;k<8;k++) crc=(crc<<1)^((crc>>31) ? 0x04c11db7u : 0);
    }
    return crc;
}
static uint32_t ej_zero_csum(uint32_t seed, uint8_t *raw, uint32_t n, uint32_t off)
{
    uint32_t saved=ej_be32(raw+off); ej_put32(raw+off,0);
    uint32_t crc=storage_ext4_crc32c(seed,raw,n); ej_put32(raw+off,saved); return crc;
}
static int ej_io(struct storage_volume *v,uint64_t block,void *data,bool write)
{
    uint32_t sectors=v->ext4.block_size/SECTOR_SIZE;
    if (!sectors || block>=v->ext4.blocks_count || block>UINT64_MAX/sectors)
        return -RELIEFOS_EIO;
    uint64_t offset=block*sectors;
    if (offset>v->ext_sector_count || sectors>v->ext_sector_count-offset ||
        v->ext_start_lba>UINT64_MAX-offset) return -RELIEFOS_EIO;
    return write ? storage_write_device(v,v->ext_start_lba+offset,sectors,data) :
                   storage_read_device(v,v->ext_start_lba+offset,sectors,data);
}
static int ej_log_io(struct storage_volume *v,uint32_t logical,void *data,bool write)
{
    struct storage_ext4_map_result map;
    struct storage_ext4_journal *j=v->ext4.journal;
    if ((uint64_t)logical*v->ext4.block_size>=j->inode.size) return -RELIEFOS_EIO;
    int ret=storage_ext4_map_block(v,v->ext4.super_view.journal_inum,&j->inode,logical,false,&map);
    if (ret<0) return ret;
    if (map.hole || map.unwritten) return -RELIEFOS_EIO;
    return ej_io(v,map.physical,data,write);
}
static bool ej_target_ok(struct storage_volume *v,uint64_t block)
{
    struct storage_ext4_journal *j=v->ext4.journal;
    if (block>=v->ext4.blocks_count) return false;
    /* Neither a replay nor a caller may overwrite the journal itself. */
    for (uint64_t pos=0;pos<j->inode.size/v->ext4.block_size;) {
        struct storage_ext4_map_result m;
        if (storage_ext4_map_block(v,v->ext4.super_view.journal_inum,&j->inode,pos,false,&m)<0 ||
            m.hole || m.unwritten || !m.length) return false;
        if (block>=m.physical && block-m.physical<m.length) return false;
        pos+=m.length;
    }
    return true;
}
static int ej_super_write(struct storage_volume *v,uint32_t start,uint32_t sequence,uint32_t head)
{
    struct storage_ext4_journal *j=v->ext4.journal;
    uint8_t *raw=j->scan.data;
    int ret=ej_log_io(v,0,raw,false); if (ret<0) return ret;
    ej_put32(raw+24,sequence); ej_put32(raw+28,start); ej_put32(raw+88,head);
    if (ej_csum(j)) ej_put32(raw+252,ej_zero_csum(~0u,raw,1024,252));
    return ej_log_io(v,0,raw,true);
}
static void ej_recover_flag_bytes(struct storage_volume *v,uint8_t *sb,bool recover)
{
    uint32_t features=ext4_get_le32(sb+96);
    features=recover ? features|4u : features&~4u; ext4_put_le32(sb+96,features);
    if (v->ext4.super_view.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)
        (void)storage_ext4_update_super_checksum(sb,1024);
}
static int ej_recover_flag(struct storage_volume *v,bool recover)
{
    uint8_t *raw=v->ext4.journal->scan.data;
    int ret=storage_read_device(v,v->ext_start_lba+2,2,raw); if (ret<0) return ret;
    ej_recover_flag_bytes(v,raw,recover);
    ret=storage_write_device(v,v->ext_start_lba+2,2,raw);
    if (!ret) {
        if (recover) v->ext4.super_view.feature_incompat|=4;
        else v->ext4.super_view.feature_incompat&=~4u;
        ret=storage_ext4_device_flush(v);
    }
    return ret;
}
static int ej_fail(struct storage_volume *v,int ret)
{
    v->ext4.fs_error=1; v->read_only_reason=STORAGE_EXT4_READ_ONLY_JOURNAL_CORRUPT;
    if (v->ext4.journal) v->ext4.journal->error=ret;
    return ret;
}

int storage_ext4_journal_open(struct storage_volume *v)
{
    if (!v) return -RELIEFOS_EINVAL;
    if (v->ext4.journal) return 0;
    if (!(v->ext4.super_view.feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL)) return 0;
    if (!v->ext4.super_view.journal_inum) return -RELIEFOS_EOPNOTSUPP;
    struct storage_ext4_journal *j=kernel_malloc(sizeof(*j));
    if (!j) return -RELIEFOS_ENOMEM;
    storage_memzero(j,sizeof(*j)); v->ext4.journal=j;
    int ret=storage_ext4_read_inode(v,v->ext4.super_view.journal_inum,&j->inode);
    if (ret<0) goto fail;
    if ((j->inode.mode & EXT2_S_IFMT)!=EXT2_S_IFREG || j->inode.size%v->ext4.block_size) {
        ret=-RELIEFOS_EIO; goto fail;
    }
    ret=ej_log_io(v,0,j->scan.data,false); if (ret<0) goto fail;
    uint8_t *sb=j->scan.data; uint32_t type=ej_be32(sb+4);
    j->maxlen=ej_be32(sb+16); j->first=ej_be32(sb+20);
    j->sequence=ej_be32(sb+24); j->start=ej_be32(sb+28); j->head=ej_be32(sb+88);
    if (ej_be32(sb)!=EJ_MAGIC || (type!=3 && type!=4) ||
        ej_be32(sb+12)!=v->ext4.block_size || !j->first || j->first>=j->maxlen ||
        j->maxlen>0x7fffffffu || (uint64_t)j->maxlen*v->ext4.block_size>j->inode.size ||
        (j->start && (j->start<j->first || j->start>=j->maxlen))) {
        ret=-RELIEFOS_EIO; goto fail;
    }
    if (type==4) {
        j->compat=ej_be32(sb+36); j->incompat=ej_be32(sb+40);
        if ((j->compat&~1u) || (j->incompat&~31u) || ej_be32(sb+44) || ej_be32(sb+64)>1 ||
            ((j->incompat&24)==24) || ((j->compat&1) && ej_csum(j))) {
            ret=-RELIEFOS_EOPNOTSUPP; goto fail;
        }
        if (ej_csum(j) && (sb[80]!=4 || ej_be32(sb+252)!=ej_zero_csum(~0u,sb,1024,252))) {
            ret=-RELIEFOS_EIO; goto fail;
        }
    }
    storage_memcpy(j->uuid,sb+48,16);
    if (type==4 && !ej_equal(j->uuid,v->ext4.super_view.uuid,16)) {
        ret=-RELIEFOS_EOPNOTSUPP; goto fail;
    }
    j->seed=storage_ext4_crc32c(~0u,j->uuid,16);
    if (j->head<j->first || j->head>=j->maxlen) j->head=j->first;
    return 0;
fail:
    kernel_free(j); v->ext4.journal=NULL; return ej_fail(v,ret);
}
void storage_ext4_journal_close(struct storage_volume *v)
{
    if (!v || !v->ext4.journal) return;
    /* This discards memory only. Clean teardown calls commit/checkpoint first. */
    storage_ext4_cache_invalidate(v);
    kernel_free(v->ext4.journal); v->ext4.journal=NULL;
}
bool storage_ext4_journal_owns(const struct storage_volume *v,uint64_t block)
{
    if (!v || !v->ext4.journal) return false;
    for (uint32_t i=0;i<v->ext4.journal->count;i++)
        if (v->ext4.journal->images[i].block==block) return true;
    return false;
}
int storage_ext4_journal_capture(struct storage_volume *v,uint64_t block,const uint8_t *data)
{
    /* New mutations are gated by journal_start. An operation already in
     * flight may still need non-journaled compensation after latching fs_error. */
    if (v->read_only_reason) return -RELIEFOS_EROFS;
    struct storage_ext4_journal *j=v->ext4.journal;
    if (!j) return (v->ext4.super_view.feature_compat&EXT4_FEATURE_COMPAT_HAS_JOURNAL) ? -RELIEFOS_EROFS : 0;
    if (!j->active || j->committed || j->error) return -RELIEFOS_EROFS;
    if (storage_ext4_journal_owns(v,block)) return 0;
    if (j->count>=j->credits || j->count>=EXT4_JOURNAL_CREDITS)
        return j->error=-RELIEFOS_ENOSPC;
    if (!ej_target_ok(v,block)) return j->error=-RELIEFOS_EIO;
    struct ext4_journal_image *image=&j->images[j->count++];
    image->block=block; image->dirty=false;
    storage_memcpy(image->before,data,v->ext4.block_size);
    storage_memcpy(image->after,data,v->ext4.block_size);
    return 0;
}
int storage_ext4_journal_publish(struct storage_volume *v,uint64_t block,const uint8_t *data)
{
    struct storage_ext4_journal *j=v->ext4.journal;
    if (!j) return 0;
    for (uint32_t i=0;i<j->count;i++) if (j->images[i].block==block) {
        storage_memcpy(j->images[i].after,data,v->ext4.block_size); j->images[i].dirty=true; return 0;
    }
    return j->error=-RELIEFOS_EIO;
}
int storage_ext4_journal_start(struct storage_volume *v,uint32_t credits,struct storage_ext4_handle *h)
{
    if (!v || !h || !credits || credits>EXT4_JOURNAL_CREDITS) return -RELIEFOS_EINVAL;
    *h=(struct storage_ext4_handle){0};
    if (v->read_only_reason || v->ext4.fs_error) return -RELIEFOS_EROFS;
    struct storage_ext4_journal *j=v->ext4.journal;
    if (!j && (v->ext4.super_view.feature_compat&EXT4_FEATURE_COMPAT_HAS_JOURNAL)) return -RELIEFOS_EROFS;
    if (j) {
        if (j->start && !j->committed) return -RELIEFOS_EROFS; /* Recovery must run first. */
        if (!j->active) {
            int ret=storage_ext4_journal_commit(v,true); if (ret<0) return ret;
            ret=storage_ext4_journal_checkpoint(v); if (ret<0) return ret;
            j->before_super=v->ext4.super_view; j->before_goal=v->ext4.next_goal_block;
            j->before_window=v->ext4.reserved_window_end; j->credits=credits; j->error=0;
        } else if (credits>j->credits) {
            j->credits=credits;
        }
        ++j->active;
    }
    *h=(struct storage_ext4_handle){.volume=v,.credits=credits,.active=true};
    return 0;
}
int storage_ext4_journal_dirty(struct storage_ext4_handle *h,uint64_t block)
{
    if (!h || !h->active) return -RELIEFOS_EINVAL;
    uint8_t *data;
    int ret=storage_ext4_cache_get(h->volume,block,&data,true);
    return ret<0 ? ret : storage_ext4_cache_mark_dirty(h->volume,block);
}
void storage_ext4_journal_abort(struct storage_ext4_handle *h,int error)
{ if (h && h->active && h->volume->ext4.journal) h->volume->ext4.journal->error=error<0 ? error : -RELIEFOS_EIO; }
int storage_ext4_journal_stop(struct storage_ext4_handle *h)
{
    if (!h || !h->active) return -RELIEFOS_EINVAL;
    h->active=false;
    struct storage_volume *v=h->volume; struct storage_ext4_journal *j=v->ext4.journal;
    if (!j) return 0;
    --j->active; int ret=j->error;
    if (!j->active && ret) {
        for (uint32_t i=0;i<j->count;i++) storage_ext4_cache_finish(v,j->images[i].block,j->images[i].before);
        v->ext4.super_view=j->before_super; v->ext4.next_goal_block=j->before_goal;
        v->ext4.reserved_window_end=j->before_window;
        storage_ext4_invalidate_groups(v); storage_ext4_cache_invalidate(v);
        j->count=0; j->error=0;
    }
    return ret;
}

static void ej_header(uint8_t *raw,uint32_t type,uint32_t seq)
{ ej_put32(raw,EJ_MAGIC); ej_put32(raw+4,type); ej_put32(raw+8,seq); }
static uint32_t ej_data_csum(struct storage_ext4_journal *j,uint32_t seq,const uint8_t *raw,uint32_t bs)
{ uint8_t word[4]; ej_put32(word,seq); return storage_ext4_crc32c(storage_ext4_crc32c(j->seed,word,4),raw,bs); }

int storage_ext4_journal_commit(struct storage_volume *v,bool wait)
{
    (void)wait; /* This backend has synchronous sector completion. */
    if (!v) return -RELIEFOS_EINVAL;
    struct storage_ext4_journal *j=v->ext4.journal;
    if (!j || !j->count || j->committed) return 0;
    if (j->active) return -RELIEFOS_EBUSY;
    if (j->error || v->ext4.fs_error || v->read_only_reason) return -RELIEFOS_EROFS;
    uint32_t bs=v->ext4.block_size, tags=0;
    for (uint32_t i=0;i<j->count;i++) if (j->images[i].dirty) ++tags;
    if (!tags) {
        for (uint32_t i=0;i<j->count;i++) storage_ext4_cache_finish(v,j->images[i].block,j->images[i].before);
        j->count=0; return 0;
    }
    if (tags+2>=j->maxlen-j->first) return -RELIEFOS_ENOSPC;
    /* Publish recovery requirement and journal start before any home metadata.
     * The flush also orders caller data writes before the metadata transaction. */
    int ret=ej_recover_flag(v,true); if (ret<0) return ej_fail(v,ret);
    j->start=j->head;
    ret=ej_super_write(v,j->start,j->sequence,j->head); if (ret<0) return ej_fail(v,ret);
    ret=storage_ext4_device_flush(v); if (ret<0) return ej_fail(v,ret);
    uint8_t *desc=j->scan.descriptor, *data=j->scan.data;
    storage_memzero(desc,bs); ej_header(desc,1,j->sequence);
    uint32_t at=12, nr=0, tag_size=ej_tag_size(j);
    for (uint32_t i=0;i<j->count;i++) if (j->images[i].dirty) {
        struct ext4_journal_image *image=&j->images[i];
        uint64_t sb_block=1024/bs;
        if (image->block==sb_block) ej_recover_flag_bytes(v,image->after+1024%bs,true);
        storage_memcpy(data,image->after,bs);
        uint32_t flags=nr ? 2 : 0;
        if (ej_be32(data)==EJ_MAGIC) { flags|=1; ej_put32(data,0); }
        if (++nr==tags) flags|=8;
        ej_put32(desc+at,(uint32_t)image->block);
        if (j->incompat & EJ_CSUM3) {
            ej_put32(desc+at+4,flags); ej_put32(desc+at+8,(uint32_t)(image->block>>32));
            ej_put32(desc+at+12,ej_data_csum(j,j->sequence,data,bs));
        } else {
            ej_put16(desc+at+6,(uint16_t)flags);
            if (j->incompat&2) ej_put32(desc+at+8,(uint32_t)(image->block>>32));
            else if (image->block>UINT32_MAX) return ej_fail(v,-RELIEFOS_EOVERFLOW);
            if (j->incompat&EJ_CSUM2) ej_put16(desc+at+4,(uint16_t)ej_data_csum(j,j->sequence,data,bs));
        }
        at+=tag_size;
        if (nr==1) { storage_memcpy(desc+at,j->uuid,16); at+=16; }
    }
    if (ej_csum(j)) ej_put32(desc+bs-4,ej_zero_csum(j->seed,desc,bs,bs-4));
    uint32_t crc=ej_crc32_be(~0u,desc,bs), pos=j->head;
    ret=ej_log_io(v,pos,desc,true); if (ret<0) return ej_fail(v,ret); pos=ej_next(j,pos);
    for (uint32_t i=0;i<j->count;i++) if (j->images[i].dirty) {
        storage_memcpy(data,j->images[i].after,bs);
        if (ej_be32(data)==EJ_MAGIC) ej_put32(data,0);
        crc=ej_crc32_be(crc,data,bs);
        ret=ej_log_io(v,pos,data,true); if (ret<0) return ej_fail(v,ret); pos=ej_next(j,pos);
    }
    ret=storage_ext4_device_flush(v); if (ret<0) return ej_fail(v,ret);
    storage_memzero(data,bs); ej_header(data,2,j->sequence);
    if (ej_csum(j)) ej_put32(data+16,ej_zero_csum(j->seed,data,bs,16));
    else if (j->compat&1) { data[12]=1; data[13]=4; ej_put32(data+16,crc); }
    ret=ej_log_io(v,pos,data,true); if (ret<0) return ej_fail(v,ret);
    ret=storage_ext4_device_flush(v); if (ret<0) return ej_fail(v,ret);
    j->head=ej_next(j,pos); j->committed=true; ++v->ext4.journal_commits; return 0;
}
int storage_ext4_journal_checkpoint(struct storage_volume *v)
{
    if (!v) return -RELIEFOS_EINVAL;
    struct storage_ext4_journal *j=v->ext4.journal;
    if (!j || !j->committed) return 0;
    if (j->active || j->error) return -RELIEFOS_EBUSY;
    for (uint32_t i=0;i<j->count;i++) if (j->images[i].dirty) {
        int ret=ej_io(v,j->images[i].block,j->images[i].after,true);
        if (ret<0) return ej_fail(v,ret);
    }
    int ret=storage_ext4_device_flush(v); if (ret<0) return ej_fail(v,ret);
    ret=ej_super_write(v,0,j->sequence+1,j->head); if (ret<0) return ej_fail(v,ret);
    ret=storage_ext4_device_flush(v); if (ret<0) return ej_fail(v,ret);
    ret=ej_recover_flag(v,false); if (ret<0) return ej_fail(v,ret);
    for (uint32_t i=0;i<j->count;i++) storage_ext4_cache_finish(v,j->images[i].block,j->images[i].after);
    /* The superblock cache must reflect the final clean recovery bit. */
    storage_ext4_cache_invalidate(v);
    ++j->sequence; j->start=0; j->count=0; j->committed=false; return 0;
}

enum ej_pass { EJ_SCAN, EJ_COLLECT, EJ_REPLAY, EJ_FIND_REVOKE };
static int ej_walk(struct storage_volume *,struct ext4_journal_scan *,enum ej_pass,
                    uint32_t,uint32_t *,uint32_t *,uint32_t *,uint64_t,uint32_t,bool *);
static void ej_record_revoke(struct storage_ext4_journal *j,uint64_t block,uint32_t seq)
{
    uint32_t slot=(uint32_t)(block^(block>>32))&(EJ_REVOKES-1);
    for (uint32_t n=0;n<EJ_REVOKES;n++) {
        struct ext4_journal_revoke *r=&j->revokes[(slot+n)&(EJ_REVOKES-1)];
        if (!r->valid || r->block==block) { *r=(struct ext4_journal_revoke){block,seq,true}; return; }
    }
    j->revoke_overflow=true;
}
static int ej_revoked(struct storage_volume *v,uint64_t block,uint32_t seq,uint32_t limit,bool *found)
{
    struct storage_ext4_journal *j=v->ext4.journal; *found=false;
    for (uint32_t n=0;n<EJ_REVOKES;n++) if (j->revokes[n].valid && j->revokes[n].block==block &&
        (int32_t)(j->revokes[n].sequence-seq)>=0) { *found=true; return 0; }
    if (!j->revoke_overflow) return 0;
    uint32_t a,b,c;
    return ej_walk(v,&j->fallback,EJ_FIND_REVOKE,limit,&a,&b,&c,block,seq,found);
}
static int ej_walk(struct storage_volume *v,struct ext4_journal_scan *w,enum ej_pass pass,
                    uint32_t limit,uint32_t *committed,uint32_t *end_seq,uint32_t *end_pos,
                    uint64_t find_block,uint32_t find_seq,bool *found)
{
    struct storage_ext4_journal *j=v->ext4.journal;
    uint32_t pos=j->start, seq=j->sequence, used=0, bs=v->ext4.block_size, crc=~0u;
    *committed=0; *end_seq=seq; *end_pos=pos;
    while (used<limit) {
        int ret=ej_log_io(v,pos,w->descriptor,false); if (ret<0) return ret;
        uint8_t *d=w->descriptor;
        if (ej_be32(d)!=EJ_MAGIC || ej_be32(d+8)!=seq) return pass==EJ_SCAN ? 0 : -RELIEFOS_EIO;
        uint32_t type=ej_be32(d+4); ++used; pos=ej_next(j,pos);
        if (type==1) {
            if (ej_csum(j) && ej_be32(d+bs-4)!=ej_zero_csum(j->seed,d,bs,bs-4)) return -RELIEFOS_EIO;
            crc=ej_crc32_be(crc,d,bs);
            uint32_t at=12, size=ej_tag_size(j), stop=bs-(ej_csum(j)?4:0);
            bool last=false;
            while (at+size<=stop) {
                uint64_t target=ej_be32(d+at);
                uint32_t flags=(j->incompat&EJ_CSUM3) ? ej_be32(d+at+4) : ej_be16(d+at+6);
                uint32_t csum=(j->incompat&EJ_CSUM3) ? ej_be32(d+at+12) : ej_be16(d+at+4);
                if (j->incompat&2) target|=(uint64_t)ej_be32(d+at+8)<<32;
                else if ((j->incompat&EJ_CSUM3) && ej_be32(d+at+8)) return -RELIEFOS_EIO;
                at+=size;
                if (!(flags&2)) {
                    if (at+16>stop || !ej_equal(d+at,j->uuid,16)) return -RELIEFOS_EIO;
                    at+=16;
                }
                if ((flags&~15u) || !ej_target_ok(v,target) || used>=limit) return -RELIEFOS_EIO;
                ret=ej_log_io(v,pos,w->data,false); if (ret<0) return ret;
                /* The scan pass only discovers complete commits. Data in an
                 * uncommitted tail can be absent after power loss. COLLECT
                 * validates every committed data block before REPLAY writes. */
                if (ej_csum(j) && pass!=EJ_SCAN) {
                    uint32_t want=ej_data_csum(j,seq,w->data,bs);
                    if (csum!=((j->incompat&EJ_CSUM3)?want:want&65535)) return -RELIEFOS_EIO;
                }
                crc=ej_crc32_be(crc,w->data,bs);
                if (pass==EJ_REPLAY) {
                    bool revoked;
                    ret=ej_revoked(v,target,seq,limit,&revoked); if (ret<0) return ret;
                    if (!revoked) {
                        if (flags&1) ej_put32(w->data,EJ_MAGIC);
                        ret=ej_io(v,target,w->data,true); if (ret<0) return ret;
                        ++v->ext4.journal_replays;
                    } else ++v->ext4.journal_revoke_hits;
                }
                ++used; pos=ej_next(j,pos);
                if (flags&8) { last=true; break; }
            }
            if (!last) return -RELIEFOS_EIO;
        } else if (type==5) {
            if (!(j->incompat&1)) return -RELIEFOS_EIO;
            if (ej_csum(j) && ej_be32(d+bs-4)!=ej_zero_csum(j->seed,d,bs,bs-4)) return -RELIEFOS_EIO;
            uint32_t n=ej_be32(d+12), size=(j->incompat&2)?8:4;
            if (n<16 || n>bs-(ej_csum(j)?4:0) || (n-16)%size) return -RELIEFOS_EIO;
            for (uint32_t at=16;at<n;at+=size) {
                uint64_t target=ej_be32(d+at);
                if (size==8) target=(target<<32)|ej_be32(d+at+4);
                if (!ej_target_ok(v,target)) return -RELIEFOS_EIO;
                if (pass==EJ_COLLECT) ej_record_revoke(j,target,seq);
                if (pass==EJ_FIND_REVOKE && target==find_block && (int32_t)(seq-find_seq)>=0) *found=true;
            }
        } else if (type==2) {
            if (ej_csum(j)) {
                if (ej_be32(d+16)!=ej_zero_csum(j->seed,d,bs,16)) return -RELIEFOS_EIO;
            } else if ((j->compat&1) && (d[12]!=1 || d[13]!=4 || ej_be32(d+16)!=crc)) return -RELIEFOS_EIO;
            ++seq; crc=~0u; *committed=used; *end_seq=seq; *end_pos=pos;
        } else return pass==EJ_SCAN ? 0 : -RELIEFOS_EIO;
    }
    return 0;
}
int storage_ext4_journal_replay(struct storage_volume *v)
{
    if (!v) return -RELIEFOS_EINVAL;
    struct storage_ext4_journal *j=v->ext4.journal;
    if (!j) return (v->ext4.super_view.feature_incompat&4) ? -RELIEFOS_EIO : 0;
    if (j->active || j->count) return -RELIEFOS_EBUSY;
    if (v->read_only_reason && v->read_only_reason!=STORAGE_EXT4_READ_ONLY_JOURNAL_NEEDS_RECOVERY)
        return -RELIEFOS_EROFS;
    if (!j->start) {
        if (!(v->ext4.super_view.feature_incompat&4)) return 0;
        int ret=ej_recover_flag(v,false); if (ret<0) return ej_fail(v,ret);
        v->read_only_reason=0; return 0;
    }
    uint32_t committed=0,end_seq=0,end_pos=0,a,b,c;
    bool found=false;
    int ret=ej_walk(v,&j->scan,EJ_SCAN,j->maxlen-j->first,&committed,&end_seq,&end_pos,0,0,&found);
    if (ret<0) return ej_fail(v,ret);
    storage_memzero(j->revokes,sizeof(j->revokes)); j->revoke_overflow=false;
    ret=ej_walk(v,&j->scan,EJ_COLLECT,committed,&a,&b,&c,0,0,&found);
    if (!ret) ret=ej_walk(v,&j->scan,EJ_REPLAY,committed,&a,&b,&c,0,0,&found);
    if (!ret) ret=storage_ext4_device_flush(v);
    if (!ret) ret=ej_super_write(v,0,end_seq,end_pos);
    if (!ret) ret=storage_ext4_device_flush(v);
    if (!ret) ret=ej_recover_flag(v,false);
    if (ret<0) return ej_fail(v,ret);
    j->sequence=end_seq; j->head=end_pos; j->start=0;
    ++v->mount_generation; storage_ext4_cache_invalidate(v);
    v->read_only_reason=0; return 0;
}

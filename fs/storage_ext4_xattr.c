/* Inline and single-block ext4 extended attributes, including shared-block
 * copy-on-write. Namespace authorization belongs to the VFS/LSM owner; this
 * backend exposes user.* and preserves other on-disk namespaces unchanged. */
#include "../drivers/bootstrap/storage/storage_internal.h"
#define AX_MAGIC 0xea020000u
struct ax_item { const uint8_t *name,*value; uint32_t size; uint8_t index,len; };
struct ax_context {
    struct ext4_inode_view in;
    uint8_t *raw,*external; struct ax_item *items;
    unsigned count,capacity,body; uint64_t table; unsigned offset;
};
static int ax_bad(struct storage_volume *v) { v->ext4.fs_error=1; return -RELIEFOS_EIO; }
static uint32_t ax_align(uint32_t n) { return (n+3)&~3u; }
static void ax_free(struct ax_context *c)
{ kernel_free(c->raw); kernel_free(c->external); kernel_free(c->items); }
static uint32_t ax_block_checksum(struct storage_volume *v,uint64_t block,const uint8_t *raw)
{
    uint8_t word[8],zero[4]={0}; ext4_put_le64(word,block);
    uint32_t crc=storage_ext4_crc32c(storage_ext4_super_csum_seed(&v->ext4.super_view),word,8);
    crc=storage_ext4_crc32c(crc,raw,16); crc=storage_ext4_crc32c(crc,zero,4);
    return storage_ext4_crc32c(crc,raw+20,v->ext4.block_size-20);
}
static int ax_block_store(struct storage_volume *v,uint64_t block,uint8_t *raw)
{
    if (v->ext4.super_view.feature_ro_compat&EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)
        ext4_put_le32(raw+16,ax_block_checksum(v,block,raw));
    uint8_t *data; int ret=storage_ext4_cache_get(v,block,&data,true); if (ret<0) return ret;
    storage_memcpy(data,raw,v->ext4.block_size); return storage_ext4_cache_mark_dirty(v,block);
}
static int ax_parse(struct storage_volume *v,struct ax_context *c,const uint8_t *raw,
                      unsigned size,unsigned first,unsigned base)
{
    unsigned at=first,start=c->count;
    while (at+4<=size && ext4_get_le32(raw+at)) {
        if (size-at<16 || ax_align(16+raw[at])>size-at || c->count>=c->capacity ||
            !raw[at+1] || ext4_get_le32(raw+at+4)) return ax_bad(v);
        unsigned len=raw[at],off=ext4_get_le16(raw+at+2); uint32_t bytes=ext4_get_le32(raw+at+8);
        if (off>size-base || bytes>size-base-off || (bytes && (off&3))) return ax_bad(v);
        for (unsigned i=0;i<len;i++) if (!raw[at+16+i]) return ax_bad(v);
        c->items[c->count++]=(struct ax_item){raw+at+16,raw+base+off,bytes,raw[at+1],len};
        at+=ax_align(16+len);
    }
    if (at+4>size) return ax_bad(v);
    for (unsigned i=start;i<c->count;i++) {
        struct ax_item *item=&c->items[i]; if (!item->size) continue;
        if (item->value<raw+at+4) return ax_bad(v);
        for (unsigned j=start;j<i;j++) if (c->items[j].size &&
            item->value<c->items[j].value+c->items[j].size && c->items[j].value<item->value+item->size)
            return ax_bad(v);
    }
    return 0;
}
static int ax_load(struct storage_volume *v,uint64_t ino,struct ax_context *c)
{
    *c=(struct ax_context){0}; int ret=storage_ext4_read_inode(v,ino,&c->in); if (ret<0) return ret;
    unsigned size=v->ext4.inode_size,bs=v->ext4.block_size;
    c->capacity=(size+bs)/16+1; c->raw=kernel_malloc(size); c->external=kernel_malloc(bs);
    c->items=kernel_malloc(c->capacity*sizeof(*c->items));
    if (!c->raw || !c->external || !c->items) return -RELIEFOS_ENOMEM;
    struct storage_ext4_group_view g; ret=storage_ext4_read_group(v,(ino-1)/v->ext4.inodes_per_group,&g);
    if (ret<0) return ret;
    uint64_t at=((ino-1)%v->ext4.inodes_per_group)*size;
    c->table=g.inode_table+at/bs; c->offset=at%bs;
    unsigned first=size<bs-c->offset?size:bs-c->offset; uint8_t *data;
    ret=storage_ext4_cache_get(v,c->table,&data,false); if (ret<0) return ret;
    storage_memcpy(c->raw,data+c->offset,first);
    if (first<size) {
        ret=storage_ext4_cache_get(v,c->table+1,&data,false); if (ret<0) return ret;
        storage_memcpy(c->raw+first,data,size-first);
    }
    c->body=128+c->in.extra_isize;
    if (c->body+8<=size && ext4_get_le32(c->raw+c->body)==AX_MAGIC) {
        ret=ax_parse(v,c,c->raw,size,c->body+4,c->body+4); if (ret<0) return ret;
    }
    if (c->in.file_acl) {
        if (c->in.file_acl<=v->ext4.first_data_block || c->in.file_acl>=v->ext4.blocks_count) return ax_bad(v);
        ret=storage_ext4_cache_read_blocks(v,c->in.file_acl,1,c->external); if (ret<0) return ret;
        if (ext4_get_le32(c->external)!=AX_MAGIC || !ext4_get_le32(c->external+4) ||
            ext4_get_le32(c->external+4)>1024 || ext4_get_le32(c->external+8)!=1) return ax_bad(v);
        if ((v->ext4.super_view.feature_ro_compat&EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) &&
            ax_block_checksum(v,c->in.file_acl,c->external)!=ext4_get_le32(c->external+16)) return ax_bad(v);
        ret=ax_parse(v,c,c->external,bs,32,0);
    }
    return ret;
}
static int ax_name(const char *name,unsigned *len)
{
    if (!name) return -RELIEFOS_EINVAL;
    const char prefix[]="user.";
    for (unsigned i=0;i<5;i++) if (name[i]!=prefix[i]) return -RELIEFOS_EOPNOTSUPP;
    *len=0; while (name[5+*len] && *len<=255) (*len)++;
    return !*len || *len>255 ? -RELIEFOS_EINVAL : 0;
}
static bool ax_equal(const uint8_t *a,const uint8_t *b,unsigned n)
{ for (unsigned i=0;i<n;i++) if (a[i]!=b[i]) return false; return true; }
static int ax_find(struct ax_context *c,const char *name,unsigned len)
{
    for (unsigned i=0;i<c->count;i++) if (c->items[i].index==1 && c->items[i].len==len &&
        ax_equal(c->items[i].name,(const uint8_t *)name+5,len)) return (int)i;
    return -1;
}
int storage_ext4_getxattr(struct storage_volume *v,uint64_t ino,const char *name,void *value,uint32_t size,uint32_t *len)
{
    unsigned n; int ret=ax_name(name,&n); if (ret<0 || !len) return ret<0?ret:-RELIEFOS_EINVAL;
    *len=0; struct ax_context c; ret=ax_load(v,ino,&c);
    if (!ret) {
        int i=ax_find(&c,name,n);
        if (i<0) ret=-61;
        else {
            *len=c.items[i].size;
            if (size && size<*len) ret=-34;
            else if (size && !value) ret=-RELIEFOS_EINVAL;
            else if (size) storage_memcpy(value,c.items[i].value,*len);
        }
    }
    ax_free(&c); return ret;
}
static uint32_t ax_hash(const struct ax_item *item,const uint8_t *value)
{
    uint32_t h=0;
    for (unsigned i=0;i<item->len;i++) h=(h<<5)^(h>>27)^item->name[i];
    for (unsigned i=0;i<ax_align(item->size);i+=4) h=(h<<16)^(h>>16)^ext4_get_le32(value+i);
    return h;
}
static bool ax_pack(uint8_t *raw,unsigned size,unsigned base,unsigned *entry,unsigned *values,
                     const struct ax_item *item,bool external)
{
    unsigned need=ax_align(16+item->len),bytes=ax_align(item->size);
    if (*entry+need+4>*values || bytes>*values-(*entry+need+4)) return false;
    *values-=bytes; if (item->size) storage_memcpy(raw+*values,item->value,item->size);
    uint8_t *e=raw+*entry; e[0]=item->len; e[1]=item->index;
    ext4_put_le16(e+2,*values-base); ext4_put_le32(e+8,item->size);
    storage_memcpy(e+16,item->name,item->len);
    if (external) ext4_put_le32(e+12,ax_hash(item,raw+*values));
    *entry+=need; (void)size; return true;
}
static int ax_release(struct storage_volume *v,uint64_t block,uint8_t *raw)
{
    unsigned refs=ext4_get_le32(raw+4);
    if (refs==1) return storage_ext4_free_blocks(v,block,1);
    ext4_put_le32(raw+4,refs-1); return ax_block_store(v,block,raw);
}
static int ax_write_inode(struct storage_volume *v,uint64_t ino,struct ax_context *c,uint8_t *raw)
{
    unsigned size=v->ext4.inode_size,bs=v->ext4.block_size,first=size<bs-c->offset?size:bs-c->offset;
    uint64_t blocks=c->in.blocks; if (c->in.flags&EXT4_HUGE_FILE_FL) blocks/=bs/512;
    ext4_put_le32(raw+104,c->in.file_acl); ext4_put_le16(raw+118,c->in.file_acl>>32);
    ext4_put_le32(raw+28,blocks); ext4_put_le16(raw+116,blocks>>32);
    struct reliefos_time_info now; if (!time_wall_clock(&now)) ext4_put_le32(raw+12,now.unix_seconds);
    int ret=storage_ext4_update_inode_checksum(raw,size,ino,c->in.generation,&v->ext4.super_view); if (ret<0) return ret;
    uint8_t *data; ret=storage_ext4_cache_get(v,c->table,&data,true); if (ret<0) return ret;
    storage_memcpy(data+c->offset,raw,first); ret=storage_ext4_cache_mark_dirty(v,c->table); if (ret<0) return ret;
    if (first<size) {
        ret=storage_ext4_cache_get(v,c->table+1,&data,true); if (ret<0) return ret;
        storage_memcpy(data,raw+first,size-first); ret=storage_ext4_cache_mark_dirty(v,c->table+1);
    }
    return ret;
}
static int ax_set(struct storage_volume *v,uint64_t ino,const char *name,const void *value,uint32_t size,uint32_t flags)
{
    unsigned n; int ret=ax_name(name,&n); if (ret<0) return ret;
    if ((flags&~7u) || flags==3 || ((flags&4) && flags!=4) || (size && !value)) return -RELIEFOS_EINVAL;
    if (size>v->ext4.block_size) return -RELIEFOS_ENOSPC;
    struct storage_ext4_handle h; ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
    struct ax_context c; ret=ax_load(v,ino,&c); uint8_t *packed=NULL;
    if (ret<0) goto done;
    int found=ax_find(&c,name,n);
    if (found>=0 && flags==1) { ret=-17; goto done; }
    if (found<0 && (flags==2 || flags==4)) { ret=-61; goto done; }
    if (found>=0) { for (unsigned i=found;i+1<c.count;i++) c.items[i]=c.items[i+1]; c.count--; }
    if (flags!=4) c.items[c.count++]=(struct ax_item){(const uint8_t *)name+5,value,size,1,n};
    /* External entries require canonical namespace/length/name ordering. */
    for (unsigned i=1;i<c.count;i++) {
        struct ax_item item=c.items[i]; unsigned j=i;
        while (j) {
            struct ax_item *p=&c.items[j-1]; int compare=(int)p->index-item.index;
            if (!compare) compare=(int)p->len-item.len;
            if (!compare) for (unsigned k=0;k<item.len && !compare;k++) compare=(int)p->name[k]-item.name[k];
            if (compare<=0) break; c.items[j]=*p; j--;
        }
        c.items[j]=item;
    }
    unsigned is=v->ext4.inode_size,bs=v->ext4.block_size;
    packed=kernel_malloc(is+bs); if (!packed) { ret=-RELIEFOS_ENOMEM; goto done; }
    storage_memcpy(packed,c.raw,is); storage_memzero(packed+is,bs);
    bool inline_ok=c.body+8<=is; if (inline_ok) storage_memzero(packed+c.body,is-c.body);
    unsigned ie=c.body+4,iv=is,be=32,bv=bs; bool body=false,block=false;
    for (unsigned i=0;i<c.count;i++) {
        if (inline_ok && ax_pack(packed,is,c.body+4,&ie,&iv,&c.items[i],false)) body=true;
        else if (ax_pack(packed+is,bs,0,&be,&bv,&c.items[i],true)) block=true;
        else { ret=-RELIEFOS_ENOSPC; goto done; }
    }
    if (body) ext4_put_le32(packed+c.body,AX_MAGIC);
    uint64_t old=c.in.file_acl,newblock=old;
    if (block) {
        uint8_t *raw=packed+is; ext4_put_le32(raw,AX_MAGIC); ext4_put_le32(raw+4,1); ext4_put_le32(raw+8,1);
        uint32_t hash=0;
        for (unsigned at=32;at<be;at+=ax_align(16+raw[at])) {
            uint32_t item=ext4_get_le32(raw+at+12); if (!item) { hash=0; break; }
            hash=(hash<<16)^(hash>>16)^item;
        }
        ext4_put_le32(raw+12,hash);
        if (!old || ext4_get_le32(c.external+4)>1) {
            uint32_t got; ret=storage_ext4_alloc_blocks(v,v->ext4.next_goal_block,1,&newblock,&got); if (ret<0) goto done;
        }
        ret=ax_block_store(v,newblock,raw); if (ret<0) goto done;
        if (!old) c.in.blocks+=bs/512;
    } else {
        newblock=0;
        if (old) { if (c.in.blocks<bs/512) { ret=ax_bad(v); goto done; } c.in.blocks-=bs/512; }
    }
    if (old && old!=newblock) { ret=ax_release(v,old,c.external); if (ret<0) goto done; }
    c.in.file_acl=newblock;
    if (!(v->ext4.super_view.feature_compat&EXT4_FEATURE_COMPAT_EXT_ATTR)) {
        uint8_t *data; ret=storage_ext4_cache_get(v,1024/bs,&data,true); if (ret<0) goto done;
        uint8_t *sb=data+1024%bs; v->ext4.super_view.feature_compat|=EXT4_FEATURE_COMPAT_EXT_ATTR;
        ext4_put_le32(sb+0x5c,v->ext4.super_view.feature_compat);
        ret=storage_ext4_update_super_checksum(sb,1024); if (ret<0) goto done;
        ret=storage_ext4_cache_mark_dirty(v,1024/bs); if (ret<0) goto done;
    }
    ret=ax_write_inode(v,ino,&c,packed);
done:
    kernel_free(packed); ax_free(&c);
    if (ret<0) storage_ext4_journal_abort(&h,ret);
    int stop=storage_ext4_journal_stop(&h); return ret<0?ret:stop;
}
int storage_ext4_setxattr(struct storage_volume *v,uint64_t ino,const char *name,const void *value,uint32_t size,uint32_t flags)
{ if (flags&~3u) return -RELIEFOS_EINVAL; return ax_set(v,ino,name,value,size,flags); }
int storage_ext4_removexattr(struct storage_volume *v,uint64_t ino,const char *name)
{ return ax_set(v,ino,name,NULL,0,4); }
int storage_ext4_drop_xattrs(struct storage_volume *v,uint64_t ino,struct ext4_inode_view *in)
{
    if (!in->file_acl) return 0;
    struct ax_context c; int ret=ax_load(v,ino,&c);
    if (!ret) {
        if (in->blocks<v->ext4.block_size/512) ret=ax_bad(v);
        else {
            ret=ax_release(v,in->file_acl,c.external);
            if (!ret) { in->blocks-=v->ext4.block_size/512; in->file_acl=0; }
        }
    }
    ax_free(&c); return ret;
}

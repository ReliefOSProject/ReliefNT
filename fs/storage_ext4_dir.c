/* Native ext-family directory records and bounded two-level HTREEs.
 * Disk layout and hash behavior follow the ext4 format specification. No
 * directory-sized table is retained: splits sort at most one leaf's entries. */
#include "../drivers/bootstrap/storage/storage_internal.h"

static int ed_bad(struct storage_volume *v) { v->ext4.fs_error=1; return -RELIEFOS_EIO; }
static unsigned ed_name_len(const char *s)
{ unsigned n=0; if (s) while (s[n] && n<=255) n++; return n; }
static bool ed_equal(const char *a,const char *b,unsigned n)
{ for (unsigned i=0;i<n;i++) if (a[i]!=b[i]) return false; return true; }
static int ed_name(const char *s,bool mutation)
{
    unsigned n=ed_name_len(s); if (!n) return -RELIEFOS_EINVAL;
    if (n>255) return -36;
    for (unsigned i=0;i<n;i++) if (s[i]=='/') return -RELIEFOS_EINVAL;
    if (mutation && s[0]=='.' && (n==1 || (n==2 && s[1]=='.'))) return -RELIEFOS_EINVAL;
    return (int)n;
}
static uint16_t ed_need(unsigned n) { return (8+n+3)&~3u; }
static uint32_t ed_seed(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in)
{
    uint8_t w[4]; uint32_t c=storage_ext4_super_csum_seed(&v->ext4.super_view);
    ext4_put_le32(w,ino); c=storage_ext4_crc32c(c,w,4);
    ext4_put_le32(w,in->generation); return storage_ext4_crc32c(c,w,4);
}
static bool ed_csum(struct storage_volume *v)
{ return (v->ext4.super_view.feature_ro_compat&EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)!=0; }
static unsigned ed_limit(struct storage_volume *v) { return v->ext4.block_size-(ed_csum(v)?12:0); }
static void ed_entry(struct storage_volume *v,uint8_t *p,unsigned rec,const char *name,
                      unsigned len,uint64_t ino,uint8_t type)
{
    storage_memzero(p,rec); ext4_put_le32(p,ino); ext4_put_le16(p+4,rec); p[6]=len;
    if (v->ext4.super_view.feature_incompat&EXT4_FEATURE_INCOMPAT_FILETYPE) p[7]=type;
    storage_memcpy(p+8,name,len);
}
static int ed_validate(struct storage_volume *v,const uint8_t *p,unsigned off,unsigned limit)
{
    if (limit-off<8) return ed_bad(v);
    unsigned rec=ext4_get_le16(p+off+4),len=p[off+6];
    if (rec<8 || (rec&3) || rec>limit-off || len>rec-8 || ext4_get_le32(p+off)>v->ext4.inodes_count ||
        ((v->ext4.super_view.feature_incompat&EXT4_FEATURE_INCOMPAT_FILETYPE)?p[off+7]>7:p[off+7]!=0))
        return ed_bad(v);
    if (ext4_get_le32(p+off) && !len) return ed_bad(v);
    if (ext4_get_le32(p+off)) for (unsigned i=0;i<len;i++)
        if (!p[off+8+i] || p[off+8+i]=='/') return ed_bad(v);
    return rec;
}
static uint32_t ed_dx_checksum(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in,
                               const uint8_t *p,unsigned off)
{
    uint32_t c=ed_seed(v,ino,in); const uint8_t zero[4]={0};
    unsigned count=ext4_get_le16(p+off+2),tail=off+8*ext4_get_le16(p+off);
    c=storage_ext4_crc32c(c,p,off+count*8); c=storage_ext4_crc32c(c,p+tail,4);
    return storage_ext4_crc32c(c,zero,4);
}
static int ed_dx_validate(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in,
                           const uint8_t *p,unsigned off)
{
    unsigned bs=v->ext4.block_size,limit=ext4_get_le16(p+off),count=ext4_get_le16(p+off+2);
    if (off==32 && (ext4_get_le16(p+4)!=12 || ext4_get_le16(p+16)!=bs-12 ||
                   ext4_get_le32(p+24) || p[29]!=8 || p[30]>1 || p[31] || p[28]>5)) return -RELIEFOS_EIO;
    if (!count || count>limit || limit!=(bs-off-(ed_csum(v)?8:0))/8) return -RELIEFOS_EIO;
    uint32_t previous=0;
    for (unsigned i=0;i<count;i++) {
        uint32_t hash=i?ext4_get_le32(p+off+i*8):0,b=ext4_get_le32(p+off+i*8+4);
        if (hash<previous || !b || (uint64_t)b*bs>=in->size || (b&0xff000000)) return -RELIEFOS_EIO;
        previous=hash;
    }
    if (ed_csum(v) && ed_dx_checksum(v,ino,in,p,off)!=ext4_get_le32(p+off+limit*8+4)) return -RELIEFOS_EIO;
    return 0;
}
static int ed_read(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in,
                    uint64_t logical,uint8_t *p,bool check_index)
{
    struct storage_ext4_map_result m; int ret=storage_ext4_map_block(v,ino,in,logical,false,&m);
    if (ret<0) return ret;
    if (m.hole || m.unwritten) return ed_bad(v);
    ret=storage_ext4_cache_read_blocks(v,m.physical,1,p); if (ret<0) return ret;
    bool root=(in->flags&EXT4_INDEX_FL) && !logical;
    bool index=(in->flags&EXT4_INDEX_FL) && logical && !ext4_get_le32(p) &&
               ext4_get_le16(p+4)==v->ext4.block_size && !p[6] && !p[7];
    if (root || index) return check_index?ed_dx_validate(v,ino,in,p,root?32:8):0;
    if (ed_csum(v)) {
        unsigned end=ed_limit(v);
        if (ext4_get_le32(p+end) || ext4_get_le16(p+end+4)!=12 || p[end+6] || p[end+7]!=0xde ||
            storage_ext4_crc32c(ed_seed(v,ino,in),p,end)!=ext4_get_le32(p+end+8)) return ed_bad(v);
    }
    return 0;
}
static int ed_store(struct storage_volume *v,uint64_t ino,struct ext4_inode_view *in,
                     uint64_t logical,uint8_t *p,unsigned dxoff)
{
    if (ed_csum(v)) {
        if (dxoff) { unsigned tail=dxoff+ext4_get_le16(p+dxoff)*8;
            ext4_put_le32(p+tail,0); ext4_put_le32(p+tail+4,ed_dx_checksum(v,ino,in,p,dxoff)); }
        else { unsigned end=ed_limit(v); storage_memzero(p+end,12); ext4_put_le16(p+end+4,12);
            p[end+7]=0xde; ext4_put_le32(p+end+8,storage_ext4_crc32c(ed_seed(v,ino,in),p,end)); }
    }
    struct storage_ext4_map_result m; int ret=storage_ext4_map_block(v,ino,in,logical,false,&m);
    if (ret<0) return ret;
    if (m.hole || m.unwritten) return ed_bad(v);
    uint8_t *data; ret=storage_ext4_cache_get(v,m.physical,&data,true); if (ret<0) return ret;
    storage_memcpy(data,p,v->ext4.block_size); return storage_ext4_cache_mark_dirty(v,m.physical);
}
static int ed_append(struct storage_volume *v,uint64_t ino,struct ext4_inode_view *in,uint64_t *logical)
{
    uint32_t n; uint64_t b; int ret=storage_ext4_alloc_blocks(v,v->ext4.next_goal_block,1,&b,&n);
    if (ret<0) return ret;
    *logical=in->size/v->ext4.block_size;
    ret=storage_ext4_change_mapping(v,ino,in,*logical,b,1,false,false); if (ret<0) return ret;
    in->size+=v->ext4.block_size; return storage_ext4_write_inode(v,ino,in);
}
static void ed_touch(struct ext4_inode_view *in)
{ struct reliefos_time_info now; if (!time_wall_clock(&now)) in->ctime=in->mtime=now.unix_seconds; }
static int ed_get_inode(struct storage_volume *v,uint64_t ino,struct ext4_inode_view *in)
{
    int ret=storage_ext4_read_inode(v,ino,in); if (ret<0) return ret;
    if ((in->mode&EXT2_S_IFMT)!=EXT2_S_IFDIR) return -20;
    if (!in->size || in->size%v->ext4.block_size) return ed_bad(v);
    return 0;
}
static int ed_stop(struct storage_ext4_handle *h,int ret)
{ if (ret<0) storage_ext4_journal_abort(h,ret); int stop=storage_ext4_journal_stop(h); return ret<0?ret:stop; }

/* Independent table-driven implementation of the specified half-MD4 rounds
 * and 16-round TEA transform. Arithmetic is modulo 2^32. */
static uint32_t ed_rot(uint32_t x,unsigned n) { return (x<<n)|(x>>(32-n)); }
int storage_ext4_dir_hash(struct storage_volume *v,const char *name,unsigned version,uint32_t *hash)
{
    uint8_t *sb; int ret=storage_ext4_cache_get(v,1024/v->ext4.block_size,&sb,false); if (ret<0) return ret;
    sb+=1024%v->ext4.block_size;
    if (version<3 && (ext4_get_le32(sb+0x160)&2)) version+=3;
    if (version>5) return -RELIEFOS_EOPNOTSUPP;
    uint32_t state[4]={0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    bool nonzero=false; for (unsigned i=0;i<4;i++) if (ext4_get_le32(sb+0xec+i*4)) nonzero=true;
    if (nonzero) for (unsigned i=0;i<4;i++) state[i]=ext4_get_le32(sb+0xec+i*4);
    bool unsign=version>=3; version%=3; unsigned len=ed_name_len(name),pos=0;
    if (!version) {
        uint32_t a=0x12a3fe2d,b=0x37abe8f9;
        while (pos<len) { int c=unsign?(uint8_t)name[pos]:(int8_t)name[pos];
            uint32_t next=b+(a^((uint32_t)c*7152373u)); if (next&0x80000000) next-=0x7fffffff;
            b=a; a=next; pos++; }
        *hash=a<<1;
    } else while (pos<len) {
        unsigned words=version==1?8:4,remaining=len-pos,take=remaining<words*4?remaining:words*4;
        uint32_t pad=remaining|(remaining<<8); pad|=pad<<16; uint32_t input[8];
        for (unsigned i=0;i<words;i++) input[i]=pad;
        for (unsigned i=0;i<take;i++) {
            int c=unsign?(uint8_t)name[pos+i]:(int8_t)name[pos+i];
            if (!(i%4) && take-i>=4) input[i/4]=0;
            input[i/4]=(input[i/4]<<8)+(uint32_t)c;
        }
        if (version==1) {
            static const uint8_t order[3][8]={{0,1,2,3,4,5,6,7},{1,3,5,7,0,2,4,6},{3,7,2,6,1,5,0,4}};
            static const uint8_t rot[3][4]={{3,7,11,19},{3,5,9,13},{3,9,11,15}};
            const uint32_t constant[3]={0,0x5a827999,0x6ed9eba1}; uint32_t s[4];
            storage_memcpy(s,state,sizeof(s));
            for (unsigned r=0;r<3;r++) for (unsigned j=0;j<8;j++) {
                unsigned a=(4-j%4)%4; uint32_t b=s[(a+1)%4],c=s[(a+2)%4],d=s[(a+3)%4];
                uint32_t f=r==0?((b&c)|(~b&d)):r==1?((b&c)|(b&d)|(c&d)):(b^c^d);
                s[a]=ed_rot(s[a]+f+input[order[r][j]]+constant[r],rot[r][j%4]);
            }
            for (unsigned i=0;i<4;i++) state[i]+=s[i]; *hash=state[1];
        } else {
            uint32_t a=state[0],b=state[1],sum=0;
            for (unsigned i=0;i<16;i++) { sum+=0x9e3779b9;
                a+=((b<<4)+input[0])^(b+sum)^((b>>5)+input[1]);
                b+=((a<<4)+input[2])^(a+sum)^((a>>5)+input[3]); }
            state[0]+=a; state[1]+=b; *hash=state[0];
        }
        pos+=take;
    }
    *hash&=~1u; if (*hash==0xfffffffe) *hash=0xfffffffc; return 0;
}
static int ed_scan(struct storage_volume *v,const uint8_t *p,unsigned limit,const char *name,
                     unsigned *position,uint64_t *ino,uint8_t *type)
{
    unsigned len=ed_name_len(name);
    for (unsigned at=0;at<limit;) {
        int rec=ed_validate(v,p,at,limit); if (rec<0) return rec;
        if (ext4_get_le32(p+at) && p[at+6]==len && ed_equal((const char *)p+at+8,name,len)) {
            if (position) *position=at; if (ino) *ino=ext4_get_le32(p+at); if (type) *type=p[at+7]; return 0;
        }
        at+=rec;
    }
    return -RELIEFOS_ENOENT;
}
static unsigned ed_dx_slot(const uint8_t *p,unsigned off,uint32_t hash)
{
    unsigned lo=1,hi=ext4_get_le16(p+off+2);
    while (lo<hi) { unsigned mid=(lo+hi)/2; if (ext4_get_le32(p+off+mid*8)<=hash) lo=mid+1; else hi=mid; }
    return lo-1;
}
struct ed_probe { uint64_t leaf,node; unsigned root_slot,node_slot,version; uint32_t hash; };
static int ed_probe(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in,
                     const char *name,uint8_t *root,uint8_t *node,struct ed_probe *out)
{
    int ret=ed_read(v,ino,in,0,root,true); if (ret<0) return ret;
    out->version=root[28]; ret=storage_ext4_dir_hash(v,name,out->version,&out->hash); if (ret<0) return ret;
    out->root_slot=ed_dx_slot(root,32,out->hash); out->node=0; out->node_slot=0;
    out->leaf=ext4_get_le32(root+36+out->root_slot*8);
    if (root[30]) {
        out->node=out->leaf; ret=ed_read(v,ino,in,out->node,node,true); if (ret<0) return ret;
        out->node_slot=ed_dx_slot(node,8,out->hash); out->leaf=ext4_get_le32(node+12+out->node_slot*8);
    }
    return 0;
}
static int ed_probe_next(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in,
                          uint8_t *root,uint8_t *node,struct ed_probe *p)
{
    uint8_t *index=p->node?node:root; unsigned off=p->node?8:32,slot=p->node?p->node_slot:p->root_slot;
    if (++slot<ext4_get_le16(index+off+2)) {
        if ((ext4_get_le32(index+off+slot*8)&~1u)!=p->hash) return 1;
        if (p->node) p->node_slot=slot; else p->root_slot=slot;
        p->leaf=ext4_get_le32(index+off+slot*8+4); return 0;
    }
    if (!p->node || p->root_slot+1>=ext4_get_le16(root+34)) return 1;
    slot=++p->root_slot;
    if ((ext4_get_le32(root+32+slot*8)&~1u)!=p->hash) return 1;
    p->node=ext4_get_le32(root+36+slot*8); p->node_slot=0;
    int ret=ed_read(v,ino,in,p->node,node,true); if (ret<0) return ret;
    p->leaf=ext4_get_le32(node+12); return 0;
}
int storage_ext4_dir_lookup(struct storage_volume *v,uint64_t ino,const char *name,uint64_t *found,uint8_t *type)
{
    int ret=ed_name(name,false); if (ret<0 || !found) return ret<0?ret:-RELIEFOS_EINVAL;
    struct ext4_inode_view in; ret=ed_get_inode(v,ino,&in); if (ret<0) return ret;
    unsigned bs=v->ext4.block_size; uint8_t *buf=kernel_malloc(3*bs); if (!buf) return -RELIEFOS_ENOMEM;
    bool indexed=(in.flags&EXT4_INDEX_FL)!=0;
    if (indexed && !(name[0]=='.' && (name[1]==0 || (name[1]=='.' && !name[2])))) {
        struct ed_probe p; ret=ed_probe(v,ino,&in,name,buf,buf+bs,&p);
        if (!ret) for (;;) {
            ret=ed_read(v,ino,&in,p.leaf,buf+2*bs,true); if (ret<0) break;
            ret=ed_scan(v,buf+2*bs,ed_limit(v),name,NULL,found,type);
            if (ret!=-RELIEFOS_ENOENT) break;
            int next=ed_probe_next(v,ino,&in,buf,buf+bs,&p);
            if (next) { if (next<0) ret=next; break; }
        }
        if (ret==0 || ret==-RELIEFOS_ENOENT) { kernel_free(buf); return ret; }
        /* Damaged index: validated leaf scanning can still serve reads, but
         * mutations remain disabled until repair/remount. */
        v->ext4.fs_error=1;
    }
    ret=-RELIEFOS_ENOENT;
    for (uint64_t l=0;l<in.size/bs;l++) {
        ret=ed_read(v,ino,&in,l,buf,false); if (ret<0) break;
        bool index=indexed && (!l || (!ext4_get_le32(buf) && ext4_get_le16(buf+4)==bs));
        ret=ed_scan(v,buf,index?bs:ed_limit(v),name,NULL,found,type);
        if (ret!=-RELIEFOS_ENOENT) break;
    }
    kernel_free(buf); return ret;
}
int storage_ext4_dir_iterate(struct storage_volume *v,uint64_t ino,uint64_t *cursor,struct storage_ext4_dirent *out)
{
    if (!cursor || !out) return -RELIEFOS_EINVAL;
    struct ext4_inode_view in; int ret=ed_get_inode(v,ino,&in); if (ret<0) return ret;
    unsigned bs=v->ext4.block_size; uint8_t *buf=kernel_malloc(bs); if (!buf) return -RELIEFOS_ENOMEM;
    ret=-RELIEFOS_ENOENT;
    while (*cursor<in.size) {
        uint64_t l=*cursor/bs; unsigned at=*cursor%bs;
        ret=ed_read(v,ino,&in,l,buf,false); if (ret<0) break;
        bool index=(in.flags&EXT4_INDEX_FL) && (!l || (!ext4_get_le32(buf) && ext4_get_le16(buf+4)==bs));
        unsigned limit=index?bs:ed_limit(v);
        if (at>=limit) { *cursor=(l+1)*bs; ret=-RELIEFOS_ENOENT; continue; }
        ret=ed_validate(v,buf,at,limit); if (ret<0) break;
        *cursor+=ret;
        if (ext4_get_le32(buf+at)) {
            out->ino=ext4_get_le32(buf+at); out->type=buf[at+7]; out->name_len=buf[at+6];
            storage_memcpy(out->name,buf+at+8,out->name_len); out->name[out->name_len]=0;
            ret=0; break;
        }
        ret=-RELIEFOS_ENOENT;
    }
    kernel_free(buf); return ret;
}

static int ed_leaf_insert(struct storage_volume *v,uint8_t *p,const char *name,uint64_t ino,uint8_t type)
{
    unsigned len=ed_name_len(name),need=ed_need(len),limit=ed_limit(v);
    for (unsigned at=0;at<limit;) {
        int rec=ed_validate(v,p,at,limit); if (rec<0) return rec;
        unsigned used=ext4_get_le32(p+at)?ed_need(p[at+6]):0;
        if ((unsigned)rec-used>=need) {
            if (used) ext4_put_le16(p+at+4,used);
            ed_entry(v,p+at+used,rec-used,name,len,ino,type); return 0;
        }
        at+=rec;
    }
    return -RELIEFOS_ENOSPC;
}
static void ed_dx_init(struct storage_volume *v,uint8_t *p,unsigned off)
{
    storage_memzero(p,v->ext4.block_size);
    if (off==8) ext4_put_le16(p+4,v->ext4.block_size);
    ext4_put_le16(p+off,(v->ext4.block_size-off-(ed_csum(v)?8:0))/8);
}
static void ed_dx_insert(uint8_t *p,unsigned off,unsigned slot,uint32_t hash,uint64_t logical)
{
    unsigned count=ext4_get_le16(p+off+2);
    for (unsigned i=count;i>slot;i--) storage_memcpy(p+off+i*8,p+off+(i-1)*8,8);
    ext4_put_le32(p+off+slot*8,hash); ext4_put_le32(p+off+slot*8+4,logical);
    ext4_put_le16(p+off+2,count+1);
}
/* Install a separator, splitting an index node or growing the root as needed. */
static int ed_index_add(struct storage_volume *v,uint64_t ino,struct ext4_inode_view *in,
                        uint8_t *root,uint8_t *node,struct ed_probe *p,uint32_t hash,uint64_t leaf,uint8_t *scratch)
{
    int ret; unsigned bs=v->ext4.block_size;
    if (!p->node && ext4_get_le16(root+34)==ext4_get_le16(root+32)) {
        uint64_t logical; ret=ed_append(v,ino,in,&logical); if (ret<0) return ret;
        ed_dx_init(v,node,8); unsigned count=ext4_get_le16(root+34);
        storage_memcpy(node+8,root+32,count*8);
        ext4_put_le16(node+8,(bs-8-(ed_csum(v)?8:0))/8);
        root[30]=1; ext4_put_le16(root+34,1); ext4_put_le32(root+36,logical);
        p->node=logical; p->node_slot=p->root_slot; p->root_slot=0;
    }
    if (!p->node) {
        ed_dx_insert(root,32,p->root_slot+1,hash,leaf); return ed_store(v,ino,in,0,root,32);
    }
    unsigned count=ext4_get_le16(node+10),limit=ext4_get_le16(node+8);
    if (count<limit) {
        ed_dx_insert(node,8,p->node_slot+1,hash,leaf);
        ret=ed_store(v,ino,in,p->node,node,8); if (ret<0) return ret;
        return ed_store(v,ino,in,0,root,32);
    }
    if (ext4_get_le16(root+34)==ext4_get_le16(root+32)) return -RELIEFOS_EFBIG;
    /* Split before inserting; both halves retain their own count/limit word. */
    unsigned left=count/2,right=count-left; uint32_t separator=ext4_get_le32(node+8+left*8);
    uint64_t newnode; ret=ed_append(v,ino,in,&newnode); if (ret<0) return ret;
    ed_dx_init(v,scratch,8); storage_memcpy(scratch+8,node+8+left*8,right*8);
    ext4_put_le16(scratch+8,limit); ext4_put_le16(scratch+10,right); ext4_put_le16(node+10,left);
    if (p->node_slot+1<left) ed_dx_insert(node,8,p->node_slot+1,hash,leaf);
    else if (p->node_slot+1==left) {
        /* New item becomes the right half's implicit first entry. */
        for (unsigned i=right;i>0;i--) storage_memcpy(scratch+8+i*8,scratch+8+(i-1)*8,8);
        ext4_put_le32(scratch+16,separator); ext4_put_le32(scratch+12,leaf);
        ext4_put_le16(scratch+8,limit); ext4_put_le16(scratch+10,right+1); separator=hash;
    } else ed_dx_insert(scratch,8,p->node_slot+1-left,hash,leaf);
    ret=ed_store(v,ino,in,p->node,node,8); if (ret<0) return ret;
    ret=ed_store(v,ino,in,newnode,scratch,8); if (ret<0) return ret;
    ed_dx_insert(root,32,p->root_slot+1,separator,newnode); return ed_store(v,ino,in,0,root,32);
}
struct ed_sort_record { uint32_t hash; uint16_t offset,len; };
static int ed_split_leaf(struct storage_volume *v,uint64_t ino,struct ext4_inode_view *in,
                         uint8_t *root,uint8_t *node,uint8_t *old,uint8_t *left,uint8_t *right,
                         struct ed_probe *probe,const char *name,uint64_t child,uint8_t type)
{
    unsigned limit=ed_limit(v),count=0,total=0;
    struct ed_sort_record *records=kernel_malloc((limit/8+1)*sizeof(*records));
    if (!records) return -RELIEFOS_ENOMEM; int ret=0;
    for (unsigned at=0;at<limit;) {
        ret=ed_validate(v,old,at,limit); if (ret<0) goto out;
        if (ext4_get_le32(old+at)) {
            char n[256]; unsigned len=old[at+6]; storage_memcpy(n,old+at+8,len); n[len]=0;
            uint32_t h; ret=storage_ext4_dir_hash(v,n,probe->version,&h); if (ret<0) goto out;
            records[count++]=(struct ed_sort_record){h,at,ed_need(len)}; total+=ed_need(len);
        }
        at+=ext4_get_le16(old+at+4);
    }
    for (unsigned i=1;i<count;i++) {
        struct ed_sort_record item=records[i]; unsigned j=i;
        while (j && records[j-1].hash>item.hash) { records[j]=records[j-1]; j--; } records[j]=item;
    }
    unsigned split=0,bytes=0;
    while (split+1<count && bytes<total/2) bytes+=records[split++].len;
    if (!split || split==count) { ret=-RELIEFOS_ENOSPC; goto out; }
    uint32_t separator=records[split].hash;
    if (records[split-1].hash==separator) separator|=1;
    storage_memzero(left,v->ext4.block_size); storage_memzero(right,v->ext4.block_size);
    unsigned positions[2]={0,0},last[2]={0,0};
    for (unsigned i=0;i<count;i++) {
        unsigned side=i>=split; uint8_t *dst=side?right:left; struct ed_sort_record r=records[i];
        storage_memcpy(dst+positions[side],old+r.offset,r.len); ext4_put_le16(dst+positions[side]+4,r.len);
        last[side]=positions[side]; positions[side]+=r.len;
    }
    ext4_put_le16(left+last[0]+4,limit-last[0]); ext4_put_le16(right+last[1]+4,limit-last[1]);
    ret=ed_leaf_insert(v,probe->hash>=(separator&~1u)?right:left,name,child,type); if (ret<0) goto out;
    uint64_t newleaf; ret=ed_append(v,ino,in,&newleaf); if (ret<0) goto out;
    ret=ed_store(v,ino,in,probe->leaf,left,0); if (ret<0) goto out;
    ret=ed_store(v,ino,in,newleaf,right,0); if (ret<0) goto out;
    ret=ed_index_add(v,ino,in,root,node,probe,separator,newleaf,old);
out:
    kernel_free(records); return ret;
}
static int ed_convert(struct storage_volume *v,uint64_t ino,struct ext4_inode_view *in,uint8_t *root,uint8_t *leaf)
{
    /* Conversion applies only to one-block linear directories. Preserve dot
     * inode values and pack all remaining entries into a new leaf. */
    unsigned limit=ed_limit(v),used=0,last=0; uint32_t parent=0; int ret;
    storage_memzero(leaf,v->ext4.block_size);
    for (unsigned at=0;at<limit;) {
        ret=ed_validate(v,root,at,limit); if (ret<0) return ret;
        unsigned len=root[at+6]; const char *name=(const char *)root+at+8;
        if (len==2 && name[0]=='.' && name[1]=='.') parent=ext4_get_le32(root+at);
        else if (ext4_get_le32(root+at) && !(len==1 && name[0]=='.')) {
            unsigned n=ed_need(len); storage_memcpy(leaf+used,root+at,n);
            ext4_put_le16(leaf+used+4,n); last=used; used+=n;
        }
        at+=ret;
    }
    if (!parent) return ed_bad(v);
    ext4_put_le16(leaf+last+4,limit-last); uint64_t logical;
    ret=ed_append(v,ino,in,&logical); if (ret<0) return ret;
    ret=ed_store(v,ino,in,logical,leaf,0); if (ret<0) return ret;
    ed_dx_init(v,root,32); ed_entry(v,root,12,".",1,ino,2);
    /* The second record covers the index but only its minimal bytes are zeroed. */
    ed_entry(v,root+12,12,"..",2,parent,2); ext4_put_le16(root+16,v->ext4.block_size-12);
    root[28]=1; root[29]=8; ext4_put_le16(root+34,1); ext4_put_le32(root+36,logical);
    in->flags|=EXT4_INDEX_FL; ret=storage_ext4_write_inode(v,ino,in); if (ret<0) return ret;
    return ed_store(v,ino,in,0,root,32);
}
static int ed_insert_impl(struct storage_volume *v,uint64_t ino,const char *name,uint64_t child,uint8_t type)
{
    int ret=ed_name(name,true); if (ret<0) return ret;
    if (!child || child>v->ext4.inodes_count || type>7) return -RELIEFOS_EINVAL;
    uint64_t existing; ret=storage_ext4_dir_lookup(v,ino,name,&existing,NULL);
    if (!ret) return -17; if (ret!=-RELIEFOS_ENOENT) return ret;
    if (v->ext4.fs_error) return -RELIEFOS_EROFS;
    struct ext4_inode_view in; ret=ed_get_inode(v,ino,&in); if (ret<0) return ret;
    unsigned bs=v->ext4.block_size; uint8_t *buf=kernel_malloc(5*bs); if (!buf) return -RELIEFOS_ENOMEM;
    if (!(in.flags&EXT4_INDEX_FL)) {
        for (uint64_t l=0;l<in.size/bs;l++) {
            ret=ed_read(v,ino,&in,l,buf,true); if (ret<0) goto out;
            ret=ed_leaf_insert(v,buf,name,child,type);
            if (!ret) { ret=ed_store(v,ino,&in,l,buf,0); goto out; }
            if (ret!=-RELIEFOS_ENOSPC) goto out;
        }
        if (in.size==bs && (v->ext4.super_view.feature_compat&EXT4_FEATURE_COMPAT_DIR_INDEX)) {
            ret=ed_convert(v,ino,&in,buf,buf+bs); if (ret<0) goto out;
        } else {
            uint64_t l; ret=ed_append(v,ino,&in,&l); if (ret<0) goto out;
            storage_memzero(buf,bs); ext4_put_le16(buf+4,ed_limit(v));
            ret=ed_leaf_insert(v,buf,name,child,type); if (!ret) ret=ed_store(v,ino,&in,l,buf,0); goto out;
        }
    }
    struct ed_probe probe; ret=ed_probe(v,ino,&in,name,buf,buf+bs,&probe); if (ret<0) goto out;
    ret=ed_read(v,ino,&in,probe.leaf,buf+2*bs,true); if (ret<0) goto out;
    ret=ed_leaf_insert(v,buf+2*bs,name,child,type);
    if (!ret) ret=ed_store(v,ino,&in,probe.leaf,buf+2*bs,0);
    else if (ret==-RELIEFOS_ENOSPC)
        ret=ed_split_leaf(v,ino,&in,buf,buf+bs,buf+2*bs,buf+3*bs,buf+4*bs,&probe,name,child,type);
out:
    if (!ret) { ed_touch(&in); ret=storage_ext4_write_inode(v,ino,&in); }
    kernel_free(buf); return ret;
}
int storage_ext4_dir_insert(struct storage_volume *v,uint64_t ino,const char *name,uint64_t child,uint8_t type)
{
    struct storage_ext4_handle h; int ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h);
    return ret<0?ret:ed_stop(&h,ed_insert_impl(v,ino,name,child,type));
}
static int ed_remove_impl(struct storage_volume *v,uint64_t ino,const char *name,uint64_t *removed,bool dotdot)
{
    int ret=ed_name(name,!dotdot); if (ret<0) return ret;
    struct ext4_inode_view in; ret=ed_get_inode(v,ino,&in); if (ret<0) return ret;
    unsigned bs=v->ext4.block_size; uint8_t *buf=kernel_malloc(bs); if (!buf) return -RELIEFOS_ENOMEM;
    ret=-RELIEFOS_ENOENT;
    for (uint64_t l=0;l<in.size/bs;l++) {
        ret=ed_read(v,ino,&in,l,buf,true); if (ret<0) break;
        bool index=(in.flags&EXT4_INDEX_FL) && (!l || (!ext4_get_le32(buf) && ext4_get_le16(buf+4)==bs));
        unsigned pos; ret=ed_scan(v,buf,index?bs:ed_limit(v),name,&pos,removed,NULL);
        if (ret==-RELIEFOS_ENOENT) continue;
        if (ret<0) break;
        if (!pos) ext4_put_le32(buf+pos,0);
        else {
            unsigned previous=0;
            while (previous+ext4_get_le16(buf+previous+4)<pos) previous+=ext4_get_le16(buf+previous+4);
            ext4_put_le16(buf+previous+4,ext4_get_le16(buf+previous+4)+ext4_get_le16(buf+pos+4));
        }
        ret=ed_store(v,ino,&in,l,buf,index?(!l?32:8):0);
        if (!ret) { ed_touch(&in); ret=storage_ext4_write_inode(v,ino,&in); } break;
    }
    kernel_free(buf); return ret;
}
int storage_ext4_dir_remove(struct storage_volume *v,uint64_t ino,const char *name,uint64_t *removed)
{
    struct storage_ext4_handle h; int ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h);
    return ret<0?ret:ed_stop(&h,ed_remove_impl(v,ino,name,removed,false));
}

static uint8_t ed_type(uint16_t mode)
{
    switch (mode&EXT2_S_IFMT) { case 0040000:return 2; case 0120000:return 7;
        case 0020000:return 3; case 0060000:return 4; case 0010000:return 5; case 0140000:return 6; default:return 1; }
}
int storage_ext4_create(struct storage_volume *v,uint64_t parent,const char *name,uint16_t mode,
                        uint32_t uid,uint32_t gid,const char *target,uint64_t *out)
{
    int ret=ed_name(name,true); if (ret<0 || !out) return ret<0?ret:-RELIEFOS_EINVAL;
    if (((mode&EXT2_S_IFMT)==EXT2_S_IFLNK)!=!!target || (target && !target[0])) return -RELIEFOS_EINVAL;
    unsigned target_len=ed_name_len(target); bool dir=(mode&EXT2_S_IFMT)==EXT2_S_IFDIR;
    if (target && target_len>=60) { target_len=0; while (target[target_len] && target_len<4096) target_len++;
        if (target_len>=v->ext4.block_size) return -36; }
    struct storage_ext4_handle h; ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
    struct ext4_inode_view par,in={0}; uint64_t ino=0,exists;
    ret=ed_get_inode(v,parent,&par); if (ret<0) goto done;
    ret=storage_ext4_dir_lookup(v,parent,name,&exists,NULL); if (!ret) { ret=-17; goto done; }
    if (ret!=-RELIEFOS_ENOENT) goto done;
    if (dir && par.links_count==65535) { ret=-31; goto done; }
    ret=storage_ext4_alloc_inode(v,dir,&ino); if (ret<0) goto done;
    in.mode=mode; in.uid=uid; in.gid=gid; in.links_count=dir?2:1;
    ed_touch(&in); in.atime=in.ctime; static uint32_t generation; in.generation=++generation+in.ctime;
    if (v->ext4.inode_size>=160) in.extra_isize=32;
    if ((v->ext4.super_view.feature_incompat&EXT4_FEATURE_INCOMPAT_EXTENTS) &&
        ((mode&EXT2_S_IFMT)==EXT2_S_IFREG || dir || (target && target_len>=60))) {
        in.flags=EXT4_EXTENTS_FL; ext4_put_le16(in.i_block_raw,0xf30a); ext4_put_le16(in.i_block_raw+4,4);
    }
    if (target && target_len<60) { in.size=target_len; storage_memcpy(in.i_block_raw,target,target_len); }
    ret=storage_ext4_write_inode(v,ino,&in); if (ret<0) goto done;
    if (dir) {
        uint64_t logical; uint8_t *buf=kernel_malloc(v->ext4.block_size); if (!buf) { ret=-RELIEFOS_ENOMEM; goto done; }
        ret=ed_append(v,ino,&in,&logical);
        if (!ret) { storage_memzero(buf,v->ext4.block_size); ed_entry(v,buf,12,".",1,ino,2);
            ed_entry(v,buf+12,ed_limit(v)-12,"..",2,parent,2); ret=ed_store(v,ino,&in,0,buf,0); }
        kernel_free(buf); if (ret<0) goto done;
    } else if (target && target_len>=60) {
        uint32_t written; ret=storage_ext4_write_file_range(v,ino,0,target,target_len,&written); if (ret<0) goto done;
    }
    ret=ed_insert_impl(v,parent,name,ino,ed_type(mode)); if (ret<0) goto done;
    if (dir) { ret=storage_ext4_read_inode(v,parent,&par); if (ret<0) goto done;
        par.links_count++; ed_touch(&par); ret=storage_ext4_write_inode(v,parent,&par); }
done:
    ret=ed_stop(&h,ret); if (!ret) *out=ino; return ret;
}
int storage_ext4_link(struct storage_volume *v,uint64_t parent,const char *name,uint64_t ino)
{
    struct storage_ext4_handle h; int ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
    struct ext4_inode_view in; ret=storage_ext4_read_inode(v,ino,&in);
    if (!ret && (in.mode&EXT2_S_IFMT)==EXT2_S_IFDIR) ret=-RELIEFOS_EPERM;
    if (!ret && in.links_count==65535) ret=-31;
    if (!ret) ret=ed_insert_impl(v,parent,name,ino,ed_type(in.mode));
    if (!ret) { in.links_count++; ed_touch(&in); ret=storage_ext4_write_inode(v,ino,&in); }
    return ed_stop(&h,ret);
}
static int ed_empty(struct storage_volume *v,uint64_t ino)
{
    struct storage_ext4_dirent entry; uint64_t cursor=0; int ret;
    while (!(ret=storage_ext4_dir_iterate(v,ino,&cursor,&entry))) {
        if (!(entry.name[0]=='.' && (!entry.name[1] || (entry.name[1]=='.' && !entry.name[2])))) return -39;
    }
    return ret==-RELIEFOS_ENOENT?0:ret;
}
static int ed_cleanup(struct storage_volume *v,uint64_t ino,bool dir)
{
    (void)dir;
    return storage_ext4_destroy_inode(v,ino);
}
int storage_ext4_unlink_held(struct storage_volume *v,uint64_t parent,const char *name,bool directory,bool held)
{
    int ret=ed_name(name,true); if (ret<0) return ret;
    struct storage_ext4_handle h; ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
    uint64_t ino=0; struct ext4_inode_view in,par;
    ret=storage_ext4_dir_lookup(v,parent,name,&ino,NULL); if (ret<0) goto done;
    ret=storage_ext4_read_inode(v,ino,&in); if (ret<0) goto done;
    bool dir=(in.mode&EXT2_S_IFMT)==EXT2_S_IFDIR;
    if (dir!=directory) { ret=directory?-20:-21; goto done; }
    if (dir) { ret=ed_empty(v,ino); if (ret<0) goto done; }
    ret=ed_remove_impl(v,parent,name,NULL,false); if (ret<0) goto done;
    if (dir) {
        ret=storage_ext4_read_inode(v,parent,&par); if (ret<0) goto done;
        if (par.links_count<2) { ret=ed_bad(v); goto done; }
        par.links_count--; ret=storage_ext4_write_inode(v,parent,&par); if (ret<0) goto done;
        in.links_count=0;
    } else if (in.links_count) in.links_count--; else { ret=ed_bad(v); goto done; }
    ed_touch(&in);
    ret=in.links_count?storage_ext4_write_inode(v,ino,&in):storage_ext4_orphan_add(v,ino,&in);
done:
    ret=ed_stop(&h,ret);
    if (!ret && !in.links_count && !held) ret=ed_cleanup(v,ino,directory);
    return ret;
}
int storage_ext4_rename_held(struct storage_volume *v,uint64_t oldparent,const char *oldname,uint64_t newparent,const char *newname,bool held)
{
    int ret=ed_name(oldname,true); if (ret<0) return ret; ret=ed_name(newname,true); if (ret<0) return ret;
    uint64_t source,target=0; struct ext4_inode_view in,replaced={0};
    ret=storage_ext4_dir_lookup(v,oldparent,oldname,&source,NULL); if (ret<0) return ret;
    ret=storage_ext4_read_inode(v,source,&in); if (ret<0) return ret;
    bool dir=(in.mode&EXT2_S_IFMT)==EXT2_S_IFDIR;
    ret=storage_ext4_dir_lookup(v,newparent,newname,&target,NULL);
    if (ret<0 && ret!=-RELIEFOS_ENOENT) return ret;
    if (!ret && source==target) return 0;
    if (!ret) {
        ret=storage_ext4_read_inode(v,target,&replaced); if (ret<0) return ret;
        if (((replaced.mode&EXT2_S_IFMT)==EXT2_S_IFDIR)!=dir) return dir?-20:-21;
        if (dir) { ret=ed_empty(v,target); if (ret<0) return ret; }
    }
    if (dir) {
        uint64_t ancestor=newparent;
        for (uint64_t steps=0;;steps++) {
            if (ancestor==source) return -RELIEFOS_EINVAL;
            if (ancestor==2) break;
            if (steps>=v->ext4.inodes_count) return ed_bad(v);
            ret=storage_ext4_dir_lookup(v,ancestor,"..",&ancestor,NULL); if (ret<0) return ret;
        }
    }
    struct storage_ext4_handle h; ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
    if (target) {
        ret=ed_remove_impl(v,newparent,newname,NULL,false); if (ret<0) goto done;
        if (dir) replaced.links_count=0; else if (replaced.links_count) replaced.links_count--; else { ret=ed_bad(v); goto done; }
        ed_touch(&replaced);
        ret=replaced.links_count?storage_ext4_write_inode(v,target,&replaced):storage_ext4_orphan_add(v,target,&replaced);
        if (ret<0) goto done;
    }
    ret=ed_remove_impl(v,oldparent,oldname,NULL,false); if (ret<0) goto done;
    ret=ed_insert_impl(v,newparent,newname,source,ed_type(in.mode)); if (ret<0) goto done;
    if (dir && oldparent!=newparent) {
        uint8_t *buf=kernel_malloc(v->ext4.block_size); if (!buf) { ret=-RELIEFOS_ENOMEM; goto done; }
        ret=ed_read(v,source,&in,0,buf,true); unsigned pos;
        if (!ret) ret=ed_scan(v,buf,(in.flags&EXT4_INDEX_FL)?v->ext4.block_size:ed_limit(v),"..",&pos,NULL,NULL);
        if (!ret) { ext4_put_le32(buf+pos,newparent); ret=ed_store(v,source,&in,0,buf,(in.flags&EXT4_INDEX_FL)?32:0); }
        kernel_free(buf); if (ret<0) goto done;
    }
    if (dir) {
        struct ext4_inode_view par;
        if (oldparent!=newparent) {
            ret=storage_ext4_read_inode(v,oldparent,&par); if (ret<0) goto done;
            if (par.links_count<2) { ret=ed_bad(v); goto done; }
            par.links_count--; ret=storage_ext4_write_inode(v,oldparent,&par); if (ret<0) goto done;
        }
        if (oldparent!=newparent || target) {
            ret=storage_ext4_read_inode(v,newparent,&par); if (ret<0) goto done;
            if (!target) { if (par.links_count==65535) { ret=-31; goto done; } par.links_count++; }
            else if (oldparent==newparent) { if (par.links_count<2) { ret=ed_bad(v); goto done; } par.links_count--; }
            ret=storage_ext4_write_inode(v,newparent,&par); if (ret<0) goto done;
        }
    }
    ed_touch(&in); ret=storage_ext4_write_inode(v,source,&in);
done:
    ret=ed_stop(&h,ret);
    if (!ret && target && !replaced.links_count && !held) ret=ed_cleanup(v,target,dir);
    return ret;
}
int storage_ext4_unlink(struct storage_volume *v,uint64_t parent,const char *name,bool directory)
{ return storage_ext4_unlink_held(v,parent,name,directory,false); }
int storage_ext4_rename(struct storage_volume *v,uint64_t op,const char *on,uint64_t np,const char *nn)
{ return storage_ext4_rename_held(v,op,on,np,nn,false); }

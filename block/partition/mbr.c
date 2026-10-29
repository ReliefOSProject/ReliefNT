/* SPDX-License-Identifier: Apache-2.0 */
#include "mbr.h"
#include <linux/errno.h>

/** @brief Decode one unaligned little-endian MBR field. */
static uint32_t get32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
/** @brief Recognize DOS extended containers, whose extents are not filesystems. */
static bool extended(uint8_t type) { return type==5 || type==0x0f || type==0x85; }
/** @brief Test complete nonzero LBA geometry without addition overflow. */
static bool inside(uint64_t first,uint64_t count,uint64_t start,uint64_t length)
{ return count && first>=start && first-start<length && count<=length-(first-start); }
/** @brief Test overlap after each extent has passed its disk-bound checks. */
static bool overlaps(const struct mbr_partition *a,const struct mbr_partition *b)
{ return a->count && b->count && a->first<b->first+b->count && b->first<a->first+a->count; }
/** @brief Decode a legacy record without consulting obsolete CHS geometry. */
static int record(const uint8_t *bytes,struct mbr_partition *out)
{
    *out=(struct mbr_partition){.first=get32(bytes+8),.count=get32(bytes+12),.type=bytes[4]};
    /* A malformed zero-length protective entry still forbids MBR fallback. */
    if(out->type==0xee)return -EOPNOTSUPP;
    if(!out->type || !out->count) {*out=(struct mbr_partition){0};return 0;}
    return bytes[0]!=0 && bytes[0]!=0x80 ? -EINVAL : 0;
}
/** @brief Parse a complete bounded DOS table into caller-owned unpublished slots. */
static int parse(int (*read)(void *,uint64_t,uint8_t[512]),void *context,uint64_t sectors,
                    struct mbr_partition entries[MBR_PARTITION_CAPACITY])
{
    uint8_t bytes[512];int rc=read(context,0,bytes);if(rc)return rc;
    if(bytes[510]!=0x55 || bytes[511]!=0xaa)return -ENOENT;
    struct mbr_partition primary[4],container={0};
    for(unsigned i=0;i<4;++i) {
        rc=record(bytes+446+i*16,&primary[i]);if(rc)return rc;
        if(!primary[i].type)continue;
        if(!inside(primary[i].first,primary[i].count,1,sectors-1))return -EINVAL;
        for(unsigned previous=0;previous<i;++previous)
            if(overlaps(&primary[i],&primary[previous]))return -EINVAL;
        if(extended(primary[i].type)) {
            if(container.type)return -EINVAL;
            container=primary[i];
        } else entries[i]=primary[i];
    }
    if(!container.type)return 0;
    uint64_t at=container.first,visited[MBR_PARTITION_CAPACITY];unsigned visits=0,slot=4;
    for(;;) {
        if(!inside(at,1,container.first,container.count))return -EINVAL;
        if(visits==MBR_PARTITION_CAPACITY)return -ENOSPC;
        for(unsigned i=0;i<visits;++i)if(visited[i]==at)return -EINVAL;
        for(unsigned i=4;i<slot;++i)
            if(inside(at,1,entries[i].first,entries[i].count))return -EINVAL;
        visited[visits++]=at;
        rc=read(context,at,bytes);if(rc)return rc;
        if(bytes[510]!=0x55 || bytes[511]!=0xaa)return -EINVAL;
        struct mbr_partition data,link;
        rc=record(bytes+446,&data);if(rc)return rc;
        rc=record(bytes+462,&link);if(rc)return rc;
        for(unsigned i=2;i<4;++i)if(bytes[446+i*16+4] && get32(bytes+446+i*16+12))return -EINVAL;
        if(data.type) {
            if(extended(data.type) || !data.first || slot==MBR_PARTITION_CAPACITY)
                return -EINVAL;
            data.first+=at;
            if(!inside(data.first,data.count,container.first,container.count))return -EINVAL;
            for(unsigned i=4;i<slot;++i)if(overlaps(&entries[i],&data))return -EINVAL;
            for(unsigned i=0;i<visits;++i)
                if(inside(visited[i],1,data.first,data.count))return -EINVAL;
            entries[slot++]=data;
        }
        if(!link.type)return 0;
        if(!extended(link.type) || !link.first)return -EINVAL;
        link.first+=container.first;
        if(!inside(link.first,link.count,container.first,container.count))return -EINVAL;
        at=link.first;
    }
}

/** @brief Publish a complete MBR snapshot or clear every partially decoded entry. */
int mbr_read_partitions(int (*read)(void *,uint64_t,uint8_t[512]),void *context,
                         uint64_t sectors,struct mbr_partition entries[MBR_PARTITION_CAPACITY])
{
    if(!read || !entries || sectors<2)return -EINVAL;
    __builtin_memset(entries,0,MBR_PARTITION_CAPACITY*sizeof(*entries));
    int rc=parse(read,context,sectors,entries);
    if(rc)__builtin_memset(entries,0,MBR_PARTITION_CAPACITY*sizeof(*entries));
    return rc;
}

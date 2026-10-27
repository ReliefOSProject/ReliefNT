/* SPDX-License-Identifier: Apache-2.0 */
#include "../../fs/initramfs/archive.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/errno.h>
static unsigned entries,trailers,gnu_entries,gnu_hard_data;
static int stop_at;
static void hex(char *at,uint32_t value)
{static const char digits[]="0123456789abcdef";for(unsigned i=0;i<8;++i)at[7-i]=digits[(value>>(i*4))&15];}
static size_t append(unsigned char *out,const char *name,uint32_t mode,const void *data,uint32_t bytes,bool crc)
{
    size_t names=strlen(name)+1,start=110+names,data_at=(start+3)&~3ul,total=(data_at+bytes+3)&~3ul;
    memset(out,0,total);memset(out,'0',110);memcpy(out,crc ? "070702" : "070701",6);
    hex((char *)out+6,123);hex((char *)out+14,mode);hex((char *)out+22,42);hex((char *)out+30,43);
    hex((char *)out+38,2);hex((char *)out+46,888);hex((char *)out+54,bytes);hex((char *)out+94,names);
    memcpy(out+110,name,names);if(bytes)memcpy(out+data_at,data,bytes);
    uint32_t sum=0;for(uint32_t i=0;i<bytes;++i)sum+=((const unsigned char *)data)[i];
    if(crc)hex((char *)out+102,sum);return total;
}
static int visit(void *context,const struct initramfs_entry *entry)
{
    assert(context==&entries);if(!entry){++trailers;return 0;}
    ++entries;if(stop_at && entries==(unsigned)stop_at)return -EAGAIN;
    assert(entry->inode==123 && entry->uid==42 && entry->gid==43 && entry->mtime==888 && entry->links==2);
    assert(!strcmp(entry->name,"sample") && entry->name_length==6 && entry->mode==0100640 &&
        entry->size==6 && !memcmp(entry->data,"hello\n",6));return 0;
}
static int visit_gnu(void *context,const struct initramfs_entry *entry)
{
    (void)context;if(!entry){++trailers;return 0;}++gnu_entries;
    if(!strcmp(entry->name,"sample") || !strcmp(entry->name,"hard")) {
        assert(entry->links==2 && (entry->mode&0777)==0640);
        if(entry->size){assert(entry->size==6 && !memcmp(entry->data,"hello\n",6));++gnu_hard_data;}
    } else if(!strcmp(entry->name,"alias"))assert(entry->size==6 && !memcmp(entry->data,"sample",6));
    else if(!strcmp(entry->name,"dir/中文"))assert(entry->size==4 && !memcmp(entry->data,"utf8",4));
    else assert(!strcmp(entry->name,"dir") && !entry->size && (entry->mode&0170000)==0040000);
    return 0;
}
int main(int argc,char **argv)
{
    assert(argc==2);unsigned char image[1024],invalid[1024];
    size_t size=append(image,"sample",0100640,"hello\n",6,false);
    assert(!initramfs_scan(image,size,visit,&entries) && entries==1 && !trailers);
    assert(!initramfs_scan(image,size,NULL,NULL));
    for(size_t length=1;length<110;++length)assert(initramfs_scan(image,length,NULL,NULL)==-ENOEXEC);
    assert(!initramfs_scan(NULL,0,NULL,NULL));assert(initramfs_scan(NULL,1,NULL,NULL)==-EINVAL);
    memcpy(invalid,image,size);invalid[6]='q';assert(initramfs_scan(invalid,size,NULL,NULL)==-ENOEXEC);
    memcpy(invalid,image,size);hex((char *)invalid+94,0);assert(initramfs_scan(invalid,size,NULL,NULL)==-ENOEXEC);
    memcpy(invalid,image,size);invalid[116]='x';assert(initramfs_scan(invalid,size,NULL,NULL)==-ENOEXEC);
    memcpy(invalid,image,size);hex((char *)invalid+54,UINT32_MAX);assert(initramfs_scan(invalid,size,NULL,NULL)==-ENOEXEC);
    size_t crc_size=append(invalid,"sample",0100640,"hello\n",6,true);
    assert(!initramfs_scan(invalid,crc_size,NULL,NULL));invalid[120]^=1;
    assert(initramfs_scan(invalid,crc_size,NULL,NULL)==-EBADMSG);
    size_t bad=append(invalid,"dir",0040755,"x",1,false);assert(initramfs_scan(invalid,bad,NULL,NULL)==-ENOEXEC);
    bad=append(invalid,"link",0120777,NULL,0,false);assert(initramfs_scan(invalid,bad,NULL,NULL)==-ENOEXEC);
    entries=0;stop_at=1;assert(initramfs_scan(image,size,visit,&entries)==-EAGAIN && entries==1);stop_at=0;
    size_t total=size+append(image+size,"TRAILER!!!",0,NULL,0,false);
    image[total++]=0;image[total++]=0;image[total++]=0;
    total+=append(image+total,"sample",0100640,"hello\n",6,true);
    entries=trailers=0;assert(!initramfs_scan(image,total,visit,&entries) && entries==2 && trailers==1);
    unsigned char gzip[]={0x1f,0x8b,0x08,0};assert(initramfs_scan(gzip,sizeof(gzip),NULL,NULL)==-EOPNOTSUPP);
    FILE *file=fopen(argv[1],"rb");assert(file && !fseek(file,0,SEEK_END));long length=ftell(file);
    assert(length>0 && !fseek(file,0,SEEK_SET));unsigned char *gnu=malloc(length);assert(gnu);
    assert(fread(gnu,1,length,file)==(size_t)length && !fclose(file));
    trailers=0;assert(!initramfs_scan(gnu,length,visit_gnu,NULL) && gnu_entries==5 && gnu_hard_data==1 && trailers==1);free(gnu);
    puts("PASS actual newc decoder: bounds/hex/NUL/type/CRC, trailers, concatenation, callback errors and independent GNU UTF8/hardlinks");
}

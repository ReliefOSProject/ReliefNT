/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/multiboot2.h>
#include "../../fs/initramfs/boot.h"
#include "../../kernel/init/init_program.h"

static const char wire[]="070701";
static unsigned mappings;
static bool reject;
bool paging_kernel_direct_map_range(uint64_t physical,uint64_t bytes)
{return !reject && physical==0x12345000 && bytes==sizeof(wire);}
void *paging_kernel_direct_map(uint64_t physical)
{assert(physical==0x12345000);++mappings;return reject ? NULL : (void *)wire;}

int main(void)
{
    const void *image=(void *)1;size_t length=99;
    assert(initramfs_boot_source(NULL,&image,&length)==0 && !image && !length && !mappings);
    struct boot_info boot={.module_count=1,.modules={{0x100000,0x60100000,"reliefos-installer-root"}}};
    assert(initramfs_boot_source(&boot,&image,&length)==0 && !image && !length && !mappings);
    boot.modules[1]=(struct boot_module){0x12345000,0x12345000+sizeof(wire),"initramfs"};boot.module_count=2;
    assert(initramfs_boot_source(&boot,&image,&length)==1 && image==wire && length==sizeof(wire) && mappings==1);
    reject=true;assert(initramfs_boot_source(&boot,&image,&length)==-22 && !image && !length);reject=false;
    boot.modules[2]=boot.modules[1];boot.module_count=3;mappings=0;
    assert(initramfs_boot_source(&boot,&image,&length)==-17 && !image && !length && !mappings);
    boot.module_count=2;boot.modules[1].end=boot.modules[1].start;
    assert(initramfs_boot_source(&boot,&image,&length)==-8 && !image && !length);
    boot.module_count=17;assert(initramfs_boot_source(&boot,&image,&length)==-22);
    assert(initramfs_boot_source(NULL,NULL,&length)==-22);
    char path[256];
    assert(!init_program_path(NULL,false,path,sizeof(path)) && !strcmp(path,"/sbin/init"));
    assert(!init_program_path(NULL,true,path,sizeof(path)) && !strcmp(path,"/init"));
    const char *cmd="root=PARTUUID=x init=/real-init rdinit=/rescue quiet";
    assert(!init_program_path(cmd,false,path,sizeof(path)) && !strcmp(path,"/real-init"));
    assert(!init_program_path(cmd,true,path,sizeof(path)) && !strcmp(path,"/rescue"));
    assert(!init_program_path("init=bad rdinit=/early",true,path,sizeof(path)) && !strcmp(path,"/early"));
    assert(!init_program_path("rdinit=bad init=/normal",false,path,sizeof(path)) && !strcmp(path,"/normal"));
    assert(!init_program_path("rdinit=/first  rdinit=/last",true,path,sizeof(path)) && !strcmp(path,"/last"));
    assert(init_program_path("init=relative",false,path,sizeof(path))==-22);
    assert(init_program_path("rdinit=",true,path,sizeof(path))==-22);
    assert(init_program_path("rdinit=/long",true,path,5)==-36);
    assert(init_program_path(NULL,true,path,3)==-36);
    puts("PASS distinct reserved newc/installer modules, physical map bounds, duplicate rejection and init/rdinit contracts");
}

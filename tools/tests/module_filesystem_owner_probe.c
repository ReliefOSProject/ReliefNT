/* SPDX-License-Identifier: Apache-2.0 */
/* Selected owned ReliefNT PID1 only; Linux hosts refuse before any mutator. */
#define MODULE_BLOCK_PROBE_ENTRY module_block_previous
#include "module_block_runtime_probe.c"
#include <sys/sysmacros.h>

/** @brief Load one exact staged native filesystem module through Linux finit_module.
 * @param path Readable matching-kernel module staged in this owned QA carrier.
 * @return None; failed operations retain the guest for diagnostics. Host guard
 * in main precedes every mutator; no host Linux module operation is attempted.
 */
static void filesystem_load(const char *path)
{
    int file=open(path,O_RDONLY|O_CLOEXEC);require(file>=0,"open staged real filesystem module");
    require(!syscall(SYS_finit_module,file,"",0),"load real filesystem module");
    require(!close(file),"close filesystem module image");
}
/** @brief Verify busy native owners remain LIVE without entering cleanup.
 * @param name Exact loaded module name. All callers hold real mounted/OFD users.
 * @return None; tests actual Linux delete_module errno, no forged owner counters.
 */
static void filesystem_busy(const char *name)
{
    errno=0;require(syscall(SYS_delete_module,name,O_NONBLOCK)==-1 &&
        (errno==EWOULDBLOCK || errno==EBUSY),"mounted filesystem prevents native unload");
}
/** @brief Exercise actual filesystem load/mount/OFD/unmount/unload and retry cycles.
 * @return None. Owns only this carrier's private root/ESP/ISO objects. Uses
 * matching real .ko bytes and physical media through the common block registry.
 * Final mounts remain for the existing broader storage/module acceptance probe.
 */
static void filesystem_owners(void)
{
    require(!mkdir("/media",0755) || errno==EEXIST,"create owned optical parent");
    require(!mkdir("/media/cdrom0",0755) || errno==EEXIST,"create owned optical mountpoint");
    int raw=open("/dev/sr0",O_RDONLY|O_CLOEXEC);require(raw>=0,"optical node exists before isofs module");
    struct stat device;require(!fstat(raw,&device) && S_ISBLK(device.st_mode) && major(device.st_rdev)==11,
        "standard optical major and block metadata");
    unsigned char pvd[2048];require(pread(raw,pvd,sizeof(pvd),16*2048)==sizeof(pvd) && !memcmp(pvd+1,"CD001",5),
        "read physical optical bytes without filesystem module");
    errno=0;require(mount("/dev/sr0","/media/cdrom0","iso9660",MS_RDONLY,NULL)==-1 && errno==ENODEV,
        "absent optical filesystem reports ENODEV while device survives");
    errno=0;require(mount("/dev/sda2","/boot","vfat",0,NULL)==-1 && errno==ENODEV,
        "absent FAT filesystem reports ENODEV without losing disk");
    require(!close(raw),"close independent optical description");
    for(unsigned cycle=0;cycle<3;++cycle) {
        filesystem_load("/vfat.ko");filesystem_load("/isofs.ko");
        require(!mount("/dev/sda2","/boot","vfat",0,NULL),"mount real modular FAT ESP");
        require(!mount("/dev/sr0","/media/cdrom0","iso9660",MS_RDONLY,NULL),"mount late modular ISO9660");
        int fat=open("/boot",O_RDONLY|O_DIRECTORY),optical=open("/media/cdrom0/koopt.qa",O_RDONLY);
        require(fat>=0 && optical>=0,"open actual mounted module-backed objects");
        filesystem_busy("vfat");filesystem_busy("isofs");
        errno=0;require(umount("/media/cdrom0")==-1 && errno==EBUSY,"held optical OFD prevents unmount");
        require(!close(fat) && !close(optical),"retire actual filesystem OFDs");
        if(cycle==2)break;
        require(!umount("/boot") && !umount("/media/cdrom0"),"unmount real FAT and optical instances");
        require(!syscall(SYS_delete_module,"vfat",O_NONBLOCK) &&
            !syscall(SYS_delete_module,"isofs",O_NONBLOCK),"unload idle real native filesystems");
        printf("MODULE_FILESYSTEM_CYCLE=%u\n",cycle+1);
    }
    filesystem_busy("vfat");filesystem_busy("isofs");
    puts("MODULE_FILESYSTEM_OWNERS actual FAT/ISO modules, absent-parser optical, raw read, mount/OFD busy, unmount and reload");
}
/** @brief Enter mutating acceptance only in the explicitly selected ReliefNT PID1.
 * @return Host refusal 77 before mutators, otherwise the shared runtime status.
 */
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT") || getpid()!=1) {
        fputs("Guest-only filesystem owner probe: refusing host mutations\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0){dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);filesystem_owners();return module_block_previous();
}

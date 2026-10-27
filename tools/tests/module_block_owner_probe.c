/* SPDX-License-Identifier: Apache-2.0 */
/* Owned ReliefNT PID1 only; the host exits77 before any module/disk mutation. */
#ifdef MODULE_BLOCK_TRANSPORT_IDE
#define MODULE_BLOCK_QA_DEVICE "/dev/sdb1"
#define OWNER_MODULE "ide"
#define OWNER_IMAGE "/ide.ko"
#define OWNER_RAW "/dev/sdb"
#elif defined(MODULE_BLOCK_TRANSPORT_AHCI)
#define MODULE_BLOCK_QA_DEVICE "/dev/sdb1"
#define OWNER_MODULE "ahci"
#define OWNER_IMAGE "/ahci.ko"
#define OWNER_RAW "/dev/sdb"
#else
#define MODULE_BLOCK_QA_DEVICE "/dev/nvme0n1p1"
#define OWNER_MODULE "nvme"
#define OWNER_IMAGE "/nvme.ko"
#define OWNER_RAW "/dev/nvme0n1"
#endif
#define MODULE_BLOCK_PROBE_ENTRY module_block_previous
#include "module_block_runtime_probe.c"

/** @brief Enter the common native probe using exactly matched staged module bytes.
 * @return None; failed guest operations retain diagnostics. The PID1/ReliefNT
 * guard in main precedes this mutator, so a host cannot enter native load/unload.
 */
static void block_owner_load(void)
{
    int file=open(OWNER_IMAGE,O_RDONLY|O_CLOEXEC);require(file>=0,"open staged real block driver");
    require(!syscall(SYS_finit_module,file,"",0),"load real block driver");
    require(!close(file),"close module snapshot source");
}
/** @brief Require actual device/OFD/mount users to reject driver destruction.
 * @return None; tests Linux delete_module busy errno, never forged counters.
 */
static void block_owner_busy(void)
{
    errno=0;require(syscall(SYS_delete_module,OWNER_MODULE,O_NONBLOCK)==-1 &&
        (errno==EWOULDBLOCK || errno==EBUSY),"OFD or mount prevents controller unload");
}
/** @brief Exercise real controller discovery, raw OFD/mount pins, STOP and reload.
 * @return None. Uses only the marker-owned extra QA disk and a newly created
 * directory on the private boot carrier. No host/real-user disk path is accepted.
 */
static void block_owner_cycles(void)
{
    errno=0;require(open(OWNER_RAW,O_RDONLY)==-1 && (errno==ENOENT || errno==ENODEV),"device absent before native driver load");
    require(!mkdir("/qa-native-block",0700),"create private block owner mountpoint");
    for(unsigned cycle=0;cycle<24;++cycle) {
        block_owner_load();
        int raw=open(OWNER_RAW,O_RDONLY|O_CLOEXEC);require(raw>=0,"open actual modular raw block device");
        int alias=dup(raw);require(alias>=0 && !close(raw),"duplicate raw OFD and close original");
        unsigned char gpt[512];require(pread(alias,gpt,sizeof(gpt),512)==sizeof(gpt) && !memcmp(gpt,"EFI PART",8),
            "read real GPT through held modular OFD");
        block_owner_busy();
        require(!mount(MODULE_BLOCK_QA_DEVICE,"/qa-native-block","ext4",MS_NOATIME,NULL),"mount physical partition on modular controller");
        require(!close(alias),"retire final raw OFD while mount retains controller");
        block_owner_busy();
        int marker_file=open("/qa-native-block/reliefnt-ko-owned",O_RDONLY);char text[sizeof(marker)]={0};
        require(marker_file>=0 && read(marker_file,text,sizeof(text))==(ssize_t)sizeof(marker)-1 &&
            !memcmp(text,marker,sizeof(marker)-1) && !close(marker_file),"verify independent owned data-disk marker");
        require(!umount("/qa-native-block"),"unmount and retire filesystem/cache/device references");
        if(cycle==23)break;
        require(!syscall(SYS_delete_module,OWNER_MODULE,O_NONBLOCK),"idle controller STOP, DMA/BAR/code cleanup");
        printf("MODULE_BLOCK_OWNER_CYCLE=%u driver=%s\n",cycle+1,OWNER_MODULE);
    }
    puts("MODULE_BLOCK_OWNERS actual module/OFD/mount busy, physical GPT/data, quiesce and reload");
}
/** @brief Gate all driver/disk mutations to the explicitly selected ReliefNT PID1.
 * @return Host refusal77, otherwise the common block/runtime acceptance status.
 */
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT") || getpid()!=1) {
        fputs("Guest-only block owner probe: refusing host mutations\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0){dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);block_owner_cycles();return module_block_previous();
}

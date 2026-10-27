/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE 1
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

static const char marker[]="ReliefNT owned initramfs runtime QA\n";
/** @brief Stop an owned guest after a failed boot invariant, retaining diagnostics.
 * @param condition Required true invariant. @param operation Stable description.
 * @return None. Only called after ReliefNT/PID1/owned-marker admission.
 */
static void require(int condition,const char *operation)
{
    if(condition)return;
    fprintf(stderr,"FAIL INITRAMFS_RUNTIME: %s errno=%d (%s)\n",operation,errno,strerror(errno));
    for(;;)pause();
}
/** @brief Match one tiny owned marker through a relative actual directory FD.
 * @param directory Retained directory fd or AT_FDCWD.
 * @param name Borrowed path. No mutation occurs during admission.
 * @return True only for the exact complete marker; every opened fd is closed.
 */
static int owned(int directory,const char *name)
{
    char bytes[sizeof(marker)]={0};int file=openat(directory,name,O_RDONLY|O_CLOEXEC);
    if(file<0)return 0;
    ssize_t length=read(file,bytes,sizeof(bytes));int closed=close(file);
    return !closed && length==sizeof(marker)-1 && !memcmp(bytes,marker,sizeof(marker)-1);
}
/** @brief Execute the actual staged static BusyBox and check its guest result.
 * @param command Applet name. @param option Optional first applet argument.
 * @param name Optional final applet argument.
 * @return None. Host/PID1/marker admission precedes all calls; child operations
 * act only on this guest's staged module tree and actual native module loader.
 */
static void busybox_command(const char *command,const char *option,const char *name)
{
    pid_t child=fork();require(child>=0,"fork actual BusyBox tool");
    if(!child) {
        if(option && name)execl("/bin/busybox","busybox",command,option,name,(char *)NULL);
        else if(option)execl("/bin/busybox","busybox",command,option,(char *)NULL);
        else execl("/bin/busybox","busybox",command,(char *)NULL);
        _exit(127);
    }
    int status;pid_t waited;do {waited=waitpid(child,&status,0);} while(waited<0 && errno==EINTR);
    require(waited==child && WIFEXITED(status) && !WEXITSTATUS(status),"actual BusyBox command completed successfully");
}
/** @brief Exercise the optional reserved installer image before any controller loads.
 * @return None. Entered only after ReliefNT/PID1/owned-root admission; the extra
 * marker is present only in the private RAM-image carrier. Native filesystem
 * ownership and coherent temporary writes are checked without a physical disk.
 */
static void early_ramdisk(void)
{
    if(access("/.qa-ram-root",F_OK))return;
    struct stat status;struct statfs filesystem;
    require(!stat("/dev/ram0",&status) && S_ISBLK(status.st_mode) && status.st_rdev==0x100,
        "reserved image appears as actual Linux ram0 before storage modules");
    errno=0;require(access("/dev/sda",F_OK)==-1 && errno==ENOENT,"RAM root needs no physical controller");
    const char *modules[]={"/ext_common.ko","/ext4.ko"};
    for(unsigned i=0;i<2;++i) {
        int file=open(modules[i],O_RDONLY|O_CLOEXEC);
        require(file>=0 && !syscall(SYS_finit_module,file,"",0) && !close(file),"load actual installer root filesystem module");
    }
    require(!mount("/dev/ram0","/newroot","ext4",MS_NOATIME,NULL),"mount complete RAM block image at LBA zero");
    require(!statfs("/newroot",&filesystem) && filesystem.f_type==0xef53 &&
        !stat("/newroot",&status) && status.st_dev==0x100,"RAM mount publishes its native filesystem and device identity");
    int data=open("/newroot/ram-root-write",O_RDWR|O_CREAT|O_EXCL,0600);char bytes[6]={0};
    require(data>=0 && write(data,"image",5)==5 && !fsync(data) && lseek(data,0,SEEK_SET)==0 &&
        read(data,bytes,5)==5 && !memcmp(bytes,"image",5) && !close(data),"RAM block filesystem supports coherent native writes");
    errno=0;require(syscall(SYS_delete_module,"ext4",O_NONBLOCK)==-1 &&
        (errno==EBUSY || errno==EWOULDBLOCK),"RAM filesystem mount pins actual module code");
    require(!unlink("/newroot/ram-root-write") && !umount("/newroot"),"retire actual RAM filesystem and transient data");
    require(!syscall(SYS_delete_module,"ext4",O_NONBLOCK) && !syscall(SYS_delete_module,"ext_common",O_NONBLOCK),
        "unused RAM root filesystem dependency unloads after final retirement");
    puts("INITRAMFS_RUNTIME reserved RAM block mounts and writes before physical controller modules");
}
/** @brief Load an optional exact module sequence from this private archive.
 * @return True for the modular-root carrier, false for an all-built-in carrier.
 * Only entered after host/PID1/ownership guards. Tests actual dependency failure
 * before correct ordering, and never substitutes a built-in root controller.
 */
static bool early_modules(void)
{
    FILE *list=fopen("/.early-modules","r");
    if(!list){require(errno==ENOENT,"optional owned early-module sequence");return false;}
    errno=0;require(access("/dev/sda",F_OK)==-1 && errno==ENOENT,"root block device is absent before its module loads");
    int file=open("/ext4.ko",O_RDONLY|O_CLOEXEC);require(file>=0,"open staged root filesystem before dependency");
    errno=0;require(syscall(SYS_finit_module,file,"",0)==-1 && errno==ENOENT,
        "actual root filesystem refuses missing ext_common dependency without corrupting loader state");
    require(!close(file),"close rejected dependency candidate");
    bool tools=access("/.qa-busybox-modprobe",F_OK)==0;
    if(tools) {
        busybox_command("depmod","-a",NULL);
        busybox_command("modprobe","serial",NULL);
        busybox_command("modprobe","-b","exfat");
        errno=0;require(access("/sys/module/exfat/initstate",F_OK)==-1 && errno==ENOENT,
            "actual modprobe blacklist suppresses module loading");
        busybox_command("modprobe","qa-parameters",NULL);
        busybox_command("rmmod","parameters",NULL);
        busybox_command("modprobe","-D","ext4");
    }
    char path[256];unsigned loaded=0;
    while(fgets(path,sizeof(path),list)) {
        size_t length=strlen(path);require(length>1 && path[length-1]=='\n' && path[0]=='/',"bounded exact early-module operand");
        path[length-1]=0;file=open(path,O_RDONLY|O_CLOEXEC);require(file>=0,"open actual archive module");
        if(tools) {
            require(!close(file),"close direct image before real modprobe dependency discovery");
            if(strcmp(path,"/ext_common.ko")) {
                char *module=path+1,*extension=strstr(module,".ko");require(extension && !extension[3],"exact native module filename");
                *extension=0;
                const char *request=!strcmp(module,"ext4") ? "qa-root-filesystem" :
                    !strcmp(module,"ahci") ? "pci:v00008086d00002922sv00001AF4sd00001100bc01sc06i01" : module;
                busybox_command("modprobe",request,NULL);*extension='.';
            }
        } else require(!syscall(SYS_finit_module,file,"",0) && !close(file),"load actual early module in dependency order");
        printf("INITRAMFS_RUNTIME_LOADED %s\n",path);++loaded;
    }
    require(!ferror(list) && !fclose(list) && loaded==9,"complete all filesystem/controller module set");
    if(tools) {
        busybox_command("lsmod",NULL,NULL);
        puts("INITRAMFS_RUNTIME BusyBox real modprobe/rmmod/lsmod, modules.dep, builtin, aliases, blacklist and quoted/ranged parameters");
    }
    puts("INITRAMFS_RUNTIME root controller and filesystem were absent and loaded from actual ko files");return true;
}
/** @brief Prove a genuine initial-memory-root PID1 can move/chroot/exec a disk root.
 * @param argc Argument count. @param argv Borrowed process arguments.
 * @return 77 on hosts/incorrect launch before any mutation; an owned successful
 * guest remains alive for its runner to capture/stop. All mount and module
 * operations stay inside this explicitly staged private QA image.
 */
int main(int argc,char **argv)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT") || getpid()!=1) {
        fputs("Guest-only initramfs probe: refusing host mutations\n",stderr);return 77;
    }
    bool after=argc==3 && !strcmp(argv[1],"--after-switch");
    int old=after ? atoi(argv[2]) : AT_FDCWD;
    if(!owned(old,after ? ".reliefnt-initramfs-qa" : "/.reliefnt-initramfs-qa")) {
        fputs("Guest-only initramfs probe: refusing unowned root\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0){dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);
    struct statfs fs;struct stat status;
    if(after) {
        require(!statfs("/",&fs) && fs.f_type==0xef53,"new actual ext-family namespace root after exec");
        require(!fstatfs(old,&fs) && fs.f_type==0x01021994,"retained old-root OFD still references memory backing");
        require(owned(old,".reliefnt-initramfs-qa"),"old-root openat remains exact after chroot and exec");
        errno=0;require(lstat("/.reliefnt-initramfs-qa",&status)==-1 && errno==ENOENT,"new root does not replay old-root pathnames");
        int data=open("/dev/shm/early-retained",O_RDONLY);char bytes[5]={0};
        require(data>=0 && read(data,bytes,4)==4 && !memcmp(bytes,"kept",4) && !close(data),"moved dev/shm retains actual data through exec");
        require(!close(old),"release final old-root user descriptor");
        puts("PASS INITRAMFS_RUNTIME newc PID1, actual root mount/MS_MOVE/chroot/exec and retained old paths");
        for(;;)pause();
    }
    require(!statfs("/",&fs) && fs.f_type==0x01021994,"PID1 really executes from newc memory root");
    early_ramdisk();
    bool modular=early_modules();
    require(!stat("/native-null",&status) && S_ISCHR(status.st_mode) && status.st_rdev==0x103,
        "unpacked special inode retains Linux rdev");
    int null=open("/native-null",O_WRONLY|O_CLOEXEC);
    require(null>=0 && write(null,"discard",7)==7 && !close(null),"unpacked null inode opens the real device by rdev");
    require(!mount(NULL,"/",NULL,MS_REMOUNT|MS_NODEV,NULL),"restrict the actual initial mount with nodev");
    errno=0;require(open("/native-null",O_RDONLY)==-1 && errno==EACCES,"nodev rejects device use while inode metadata remains visible");
    require(!mount(NULL,"/",NULL,MS_REMOUNT,NULL),"restore initial mount device policy");
    errno=0;require(open("/unknown-node",O_RDONLY)==-1 && errno==ENXIO,"unbound device number returns ENXIO");
    int module=open("/standalone.ko",O_RDONLY|O_CLOEXEC);
    require(module>=0 && !syscall(SYS_finit_module,module,"",0) && !close(module),"load matching actual ko from initial memory root");
    require(!syscall(SYS_delete_module,"standalone",O_NONBLOCK),"unload unused actual early module");
    puts("INITRAMFS_RUNTIME early PID1 and actual module load/unload");
    old=open("/",O_RDONLY|O_DIRECTORY);require(old>=0,"retain initial-root descriptor across exec");
    int data=open("/dev/shm/early-retained",O_WRONLY|O_CREAT|O_EXCL,0600);
    require(data>=0 && write(data,"kept",4)==4 && !close(data),"write distinct early shm backing");
    require(!mount("/native-root","/newroot","ext4",0,NULL),"mount physical root using unpacked block inode and actual Linux rdev");
    if(modular) {
        const char *owners[]={"ext4","ext_common","ahci"};
        for(unsigned i=0;i<sizeof(owners)/sizeof(owners[0]);++i) {
            errno=0;require(syscall(SYS_delete_module,owners[i],O_NONBLOCK)==-1 &&
                (errno==EWOULDBLOCK || errno==EBUSY),"real-root filesystem/dependency/controller owners reject busy unload");
        }
    }
    const char *providers[]={"/dev","/proc","/sys"};
    for(unsigned i=0;i<sizeof(providers)/sizeof(providers[0]);++i) {
        char destination[64];snprintf(destination,sizeof(destination),"/newroot%s",providers[i]);
        require(!mount(providers[i],destination,NULL,MS_MOVE,NULL),"move exact provider subtree into actual new root");
    }
    require(!chdir("/newroot") && !mount(".","/",NULL,MS_MOVE,NULL),"move real mount over initial root");
    require(!chroot(".") && !chdir("/"),"switch task root/cwd by actual chroot objects");
    char number[24];snprintf(number,sizeof(number),"%d",old);
    execl("/qa-init","/qa-init","--after-switch",number,(char *)NULL);
    require(0,"exec final disk-root image preserving PID1");return 1;
}

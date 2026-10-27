/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/vmwgfx_drm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/** @brief Stop an owned private guest after reporting one failed contract.
 * @param operation Stable diagnostic name. No host-side operation is performed.
 * Task context after OS/PID1/owned-root admission, never returns.
 */
static _Noreturn void fail(const char *operation)
{fprintf(stderr,"FAIL MODULE_DRM %s errno=%d\n",operation,errno);if(getpid()!=1)_exit(1);for(;;)pause();}
/** @brief Require nonblocking unload refusal without retiring an in-use module.
 * @param operation Diagnostic label. Already admitted private guest only.
 */
static void require_busy(const char *operation)
{errno=0;if(syscall(SYS_delete_module,"drm",O_NONBLOCK)!=-1 || errno!=EWOULDBLOCK)fail(operation);}
/** @brief Resolve this actual SVGA PCI alias with the real staged target modprobe.
 * Guest-only task context; uses sysfs discovery and native modules.alias, with
 * no replacement dependency resolver or production policy embedded in the test.
 */
static void load_modalias(void)
{
    DIR *directory=opendir("/sys/bus/pci/devices");if(!directory)fail("open PCI namespace");
    char alias[256]={0};struct dirent *entry;
    while((entry=readdir(directory))) {
        if(entry->d_name[0]=='.')continue;
        char path[512];int length=snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/modalias",entry->d_name);
        if(length<0 || (size_t)length>=sizeof(path))fail("PCI alias path bounds");
        int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)fail("open actual PCI alias");
        size_t used=0;
        for(;;) {
            ssize_t got=read(fd,alias+used,sizeof(alias)-1-used);
            if(got<0)fail("read actual PCI alias");if(!got)break;used+=(size_t)got;
            if(used==sizeof(alias)-1)fail("PCI alias bounds");
        }
        close(fd);alias[used]=0;
        static const char expected[]="pci:v000015ADd00000405";
        if(!strncmp(alias,expected,sizeof(expected)-1))break;
        alias[0]=0;
    }
    closedir(directory);if(!alias[0])fail("find actual SVGA PCI alias");
    alias[strcspn(alias,"\r\n")]=0;
    pid_t child=fork();if(child<0)fail("fork alias modprobe");
    if(!child){execl("/qa-busybox","busybox","modprobe","-b",alias,(char *)NULL);_exit(127);}
    int status;if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status))fail("native PCI alias modprobe");
}
/** @brief Let real queues progress until nonblocking unload succeeds or times out.
 * Already admitted guest task context; no forced unload or fixed sleep is used.
 */
static void unload_idle(void)
{
    struct timespec now,deadline;if(clock_gettime(CLOCK_MONOTONIC,&deadline))fail("unload deadline");
    deadline.tv_sec+=5;
    for(;;) {
        if(!syscall(SYS_delete_module,"drm",O_NONBLOCK))return;
        if(errno!=EWOULDBLOCK)fail("idle DRM unload");
        if(clock_gettime(CLOCK_MONOTONIC,&now))fail("unload clock");
        if(now.tv_sec>deadline.tv_sec || (now.tv_sec==deadline.tv_sec && now.tv_nsec>=deadline.tv_nsec))fail("DRM retirement did not drain");
        sched_yield();
    }
}
/** @brief Retain actual BO mappings after FD close, then unload after cross-CPU release.
 * @param fd Owned open card, consumed. Guest task context after all contract
 * children exit. The final child verifies the original mapped bytes and drops
 * its last provider/VMA reference before requesting normal module unload.
 */
static void mapping_lifetime(int fd)
{
    require_busy("open card pins module");
    int duplicate=dup(fd);if(duplicate<0 || close(fd))fail("duplicate card OFD");
    require_busy("duplicated card pins module");fd=duplicate;
    struct drm_mode_create_dumb create={.width=64,.height=64,.bpp=32};
    if(ioctl(fd,DRM_IOCTL_MODE_CREATE_DUMB,&create) || create.size<4096)fail("create module-owned BO");
    struct drm_mode_map_dumb map={.handle=create.handle};
    if(ioctl(fd,DRM_IOCTL_MODE_MAP_DUMB,&map))fail("BO mapping offset");
    void *pixels=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off_t)map.offset);
    if(pixels==MAP_FAILED || close(fd))fail("map BO and close last descriptor");
    *(volatile unsigned *)pixels=0x12345678u;require_busy("VMA after FD close pins module");
    cpu_set_t available;CPU_ZERO(&available);
    if(sched_getaffinity(0,sizeof(available),&available))fail("DRM CPU affinity");
    int first=-1,last=-1;for(int i=0;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&available)){if(first<0)first=i;last=i;}
    if(first<0 || last<0)fail("empty DRM CPU affinity");
    int barrier[2];if(pipe(barrier))fail("DRM VMA barrier");
    pid_t child=fork();if(child<0)fail("fork mapped BO owner");
    if(!child) {
        close(barrier[1]);char go;if(read(barrier[0],&go,1)!=1)fail("wait final DRM VMA owner");close(barrier[0]);
        cpu_set_t target;CPU_ZERO(&target);CPU_SET(last,&target);
        if(last<0 || sched_setaffinity(0,sizeof(target),&target))fail("final DRM VMA affinity");
        if(*(volatile unsigned *)pixels!=0x12345678u || munmap(pixels,4096))fail("child final DRM VMA release");
        unload_idle();_exit(0);
    }
    close(barrier[0]);
    cpu_set_t target;CPU_ZERO(&target);CPU_SET(first,&target);
    if(first<0 || sched_setaffinity(0,sizeof(target),&target) || munmap(pixels,4096))fail("parent DRM VMA release");
    require_busy("forked VMA pins original module");
    if(write(barrier[1],"x",1)!=1 || close(barrier[1]))fail("release final DRM VMA owner");
    int status;if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status))fail("DRM final release child");
}
/** @brief Drive actual existing DRM syscall probes after production switch_root.
 * @return 77 before mutations on any host or unowned root; the owned VM pauses
 * after success/failure for its runner. Children use ordinary Linux/POSIX APIs;
 * no mock driver or replacement initramfs policy is used.
 */
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT") || getpid()!=1) {
        fputs("Guest-only DRM coordinator: refusing host mutations\n",stderr);return 77;
    }
    static const char expected[]="ReliefNT owned boot runtime QA\n";
    char marker[sizeof(expected)];int fd=open("/ko-runtime-owned",O_RDONLY|O_CLOEXEC);
    ssize_t count=fd<0 ? -1 : read(fd,marker,sizeof(marker));if(fd>=0)close(fd);
    if(count!=sizeof(expected)-1 || memcmp(marker,expected,sizeof(expected)-1)) {
        fputs("Guest-only DRM coordinator: refusing unowned root\n",stderr);return 77;
    }
    fd=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(fd>=0){dup2(fd,1);dup2(fd,2);if(fd>2)close(fd);}
    setvbuf(stdout,NULL,_IONBF,0);
    struct statfs fs;if(statfs("/",&fs) || fs.f_type!=0xef53)fail("production real root");
    bool modular=access("/drm.ko",F_OK)==0;
    for(unsigned cycle=0;cycle<(modular ? 8u : 1u);++cycle) {
    if(modular) {
        struct stat node;errno=0;
        if(!stat("/dev/dri/card0",&node) || errno!=ENOENT)fail("unloaded DRM has no primary node");
        load_modalias();
    }
    fd=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);if(fd<0)fail("open actual card");
    struct drm_vmw_getparam_arg capability={.param=DRM_VMW_PARAM_3D};
    unsigned long request=DRM_IOWR(DRM_COMMAND_BASE+DRM_VMW_GET_PARAM,struct drm_vmw_getparam_arg);
    if(ioctl(fd,request,&capability) || capability.value>1)fail("native VGPU9 capability");
    close(fd);
    const char *modes[]={"expect-present","expect-bo","expect-kms","expect-prime","expect-resources",
        "positional-pread64","positional-preadv","positional-preadv2","positional-preadv2-current",
        "positional-preadv2-hipri","positional-preadv2-current-hipri","positional-preadv-zero-negative",
        "positional-preadv2-zero-negative","positional-lseek","positional-interruptible"};
    for(size_t i=0;i<sizeof(modes)/sizeof(*modes);++i) {
        if(!capability.value && (!strcmp(modes[i],"expect-prime") || !strcmp(modes[i],"expect-resources"))) {
            printf("SKIP MODULE_DRM_3D %s: device reports VGPU9 unavailable\n",modes[i]);continue;
        }
        printf("MODULE_DRM_BEGIN %s\n",modes[i]);
        pid_t child=fork();if(child<0)fail("fork contract child");
        if(!child){execl("/qa-drm-probe","drm-probe",modes[i],(char *)NULL);_exit(127);}
        int status;if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status))fail(modes[i]);
    }
    if(modular) {
        fd=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);if(fd<0)fail("open card for owner lifetime");
        mapping_lifetime(fd);
        struct stat node;errno=0;
        if(!stat("/dev/dri/card0",&node) || errno!=ENOENT)fail("retired DRM primary is absent");
        fd=open("/dev/fb0",O_RDWR|O_CLOEXEC);if(fd<0 || close(fd))fail("permanent framebuffer survives DRM unload");
        printf("MODULE_DRM_CYCLE=%u native alias load, OFD/VMA/fork ownership and final unload\n",cycle+1);
    }
    }
    puts("PASS MODULE_DRM actual file ops, BO mmap/splits/fork/mremap, KMS and vector/positional Linux ABI");
    for(;;)pause();
}

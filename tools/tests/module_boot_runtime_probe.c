/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE 1
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/** @brief Stop this owned guest on a failed real early-userspace invariant.
 * @param value Required true result. @param name Borrowed operation description.
 * @return None. Invoked only after all host/PID1/marker admission checks.
 */
static void require(int value,const char *name)
{
    if(value)return;
    fprintf(stderr,"FAIL MODULE_BOOT: %s errno=%d (%s)\n",name,errno,strerror(errno));
    for(;;)pause();
}
#ifdef RELIEFNT_PARAMETER_RUNTIME_TEST
#include "module_parameter_runtime_checks.h"
#endif

/** @brief Validate a production /init transition without replacing its boot policy.
 * @return 77 on a host/unowned root before mutations; success/failure pauses the
 * admitted private VM for its runner. Filesystem writes are confined to that
 * VM snapshot; native devices/providers, volatile state and boot controls are
 * observed using ordinary Linux/POSIX interfaces.
 */
int main(void)
{
    struct utsname identity;char marker[64];
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT") || getpid()!=1) {
        fputs("Guest-only boot probe: refusing host mutations\n",stderr);return 77;
    }
    int file=open("/ko-runtime-owned",O_RDONLY|O_CLOEXEC);
    ssize_t length=file<0 ? -1 : read(file,marker,sizeof(marker));if(file>=0)close(file);
    static const char expected[]="ReliefNT owned boot runtime QA\n";
    if(length!=sizeof(expected)-1 || memcmp(marker,expected,sizeof(expected)-1)) {
        fputs("Guest-only boot probe: refusing unowned root\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0){dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);
    struct statfs fs;struct stat status;
    require(!statfs("/",&fs) && fs.f_type==0xef53,"real ext-family root after production switch_root");
    require(!statfs("/run",&fs) && fs.f_type==0x01021994 && !stat("/run",&status) &&
        (status.st_mode&07777)==0755,"fresh tmpfs /run with standard mode");
    errno=0;require(lstat("/run/ko-stale",&status)==-1 && errno==ENOENT,"stale disk runtime tree is hidden");
    require(!statfs("/dev/shm",&fs) && fs.f_type==0x01021994,"real shared-memory subtree moved intact");
    require(!stat("/dev/null",&status) && S_ISCHR(status.st_mode) && status.st_rdev==0x103,"native device namespace moved");
#ifdef RELIEFNT_ALLMOD_RUNTIME_TEST
    pid_t loader=fork();require(loader>=0,"fork actual module startup policy");
    if(!loader){execl("/usr/lib/reliefos/modules-load","modules-load",(char *)NULL);_exit(127);}
    int loader_status;
    require(waitpid(loader,&loader_status,0)==loader && WIFEXITED(loader_status) && !WEXITSTATUS(loader_status),
        "actual modules-load.d and PCI coldplug policy");
#endif
    file=open("/proc/modules",O_RDONLY|O_CLOEXEC);char modules[4096]={0};
    length=file<0 ? -1 : read(file,modules,sizeof(modules)-1);if(file>=0)close(file);
#ifdef RELIEFNT_NO_MODULES_RUNTIME_TEST
    require(file<0 && errno==ENOENT,"CONFIG_MODULES=n omits proc module inventory");
#endif
    int builtin=open("/ko-boot-builtin",O_RDONLY|O_CLOEXEC);
    bool built_in=false;
    if(builtin>=0) {
        char mode[64];ssize_t got=read(builtin,mode,sizeof(mode));close(builtin);
        static const char expected_mode[]="ReliefNT owned all-builtin boot QA\n";
        require(got==sizeof(expected_mode)-1 && !memcmp(mode,expected_mode,sizeof(expected_mode)-1),
            "exact all-builtin test expectation marker");
        built_in=true;
    }
#ifdef RELIEFNT_NO_MODULES_RUNTIME_TEST
    require(built_in && length==-1,"module-disabled kernel uses the builtin boot contract");
    errno=0;require(syscall(SYS_init_module,NULL,0,"")==-1 && errno==ENOSYS,"disabled init_module returns ENOSYS");
    errno=0;require(syscall(SYS_finit_module,-1,"",0)==-1 && errno==ENOSYS,"disabled finit_module returns ENOSYS");
    errno=0;require(syscall(SYS_delete_module,"absent",0)==-1 && errno==ENOSYS,"disabled delete_module returns ENOSYS");
    puts("PASS MODULE_DISABLED all production components builtin, no proc modules, three Linux module calls return ENOSYS");
#else
    require(built_in ? length==0 : length>0 && strstr(modules,"ahci ") && strstr(modules,"ext4 ") && strstr(modules,"ext_common "),
        "production init module state matches the explicitly staged builtin or modular contract");
#endif
#ifdef RELIEFNT_ALLMOD_RUNTIME_TEST
    require(strstr(modules,"drm ") && strstr(modules,"snd ") && strstr(modules,"hda ") &&
        strstr(modules,"e1000 ") && strstr(modules,"serial "),"late PCI/platform modules are loaded after the root switch");
    require(!stat("/dev/dri/card0",&status) && S_ISCHR(status.st_mode) &&
        !stat("/dev/snd/controlC0",&status) && S_ISCHR(status.st_mode),"actual graphics and ALSA nodes after coldplug");
    const char *graphics_modes[]={"expect-bo","expect-kms"};
    for(unsigned i=0;i<sizeof(graphics_modes)/sizeof(*graphics_modes);++i) {
        pid_t child=fork();require(child>=0,"fork allmod DRM client");
        if(!child){execl("/qa-drm-probe","drm-probe",graphics_modes[i],(char *)NULL);_exit(127);}
        int child_status;
        require(waitpid(child,&child_status,0)==child && WIFEXITED(child_status) && !WEXITSTATUS(child_status),
            "allmod actual BO and KMS clients");
    }
    pid_t player=fork();require(player>=0,"fork allmod ALSA client");
    if(!player){execl("/qa-audio-player","audio-player","--mode","play","--backend","alsa","--frames","4096",(char *)NULL);_exit(127);}
    int player_status;
    require(waitpid(player,&player_status,0)==player && WIFEXITED(player_status) && !WEXITSTATUS(player_status),
        "allmod native ALSA playback");
    puts("PASS MODULE_ALLMOD actual root modules, modules-load.d platform policy, PCI alias graphics/audio/network coldplug");
#endif
#ifdef RELIEFNT_SOUND_DISABLED_RUNTIME_TEST
    require(!strstr(modules,"snd ") && !strstr(modules,"hda ") && !strstr(modules,"ac97 ") && !strstr(modules,"es1371 "),
        "disabled sound has no loaded core or controller");
    const char *sound_nodes[]={"/dev/snd/timer","/dev/snd/controlC0","/dev/snd/pcmC0D0p"};
    for(unsigned i=0;i<sizeof(sound_nodes)/sizeof(*sound_nodes);++i) {
        errno=0;require(stat(sound_nodes[i],&status)<0 && errno==ENOENT,"disabled sound exposes no stale device nodes");
    }
    errno=0;file=open("/dev/dsp",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
    require(file<0 && errno==ENODEV,"static OSS compatibility node has no bound provider");
    puts("PASS MODULE_SOUND_DISABLED hardware present, no sound modules or ALSA devices, unbound static OSS returns ENODEV");
#endif
#ifdef RELIEFNT_DRM_DISABLED_RUNTIME_TEST
    require(!strstr(modules,"drm "),"disabled DRM has no loaded provider");
    const char *drm_nodes[]={"/dev/dri/card0","/dev/dri/renderD128"};
    for(unsigned i=0;i<sizeof(drm_nodes)/sizeof(*drm_nodes);++i) {
        errno=0;require(stat(drm_nodes[i],&status)<0 && errno==ENOENT,"disabled DRM exposes no card/render nodes");
    }
    file=open("/dev/fb0",O_RDWR|O_CLOEXEC);
    require(file>=0 && !close(file),"permanent framebuffer works without DRM");
    puts("PASS MODULE_DRM_DISABLED no module or DRM nodes, permanent framebuffer remains usable");
#endif
    require(!statfs("/boot",&fs) && fs.f_type==0x4d44,"root fstab ESP is mounted through vfat module");
    errno=0;require(lstat("/boot/reliefos/state/kerneldebug.next",&status)==-1 && errno==ENOENT,
        "deferred boot checkpoint consumed actual one-shot ESP control");
    file=open("/ko-production-init-write",O_WRONLY|O_CREAT|O_EXCL,0600);
    require(file>=0 && write(file,"standard",8)==8 && !fsync(file) && !close(file) &&
        !unlink("/ko-production-init-write"),"ordinary real-root file writes work after production init");
#ifdef RELIEFNT_PARAMETER_RUNTIME_TEST
    parameter_runtime_checks();
#endif
    puts("PASS MODULE_BOOT production newc /init, matching root/module state, fstab ESP, switch_root and runtime state");
    for(;;)pause();
}

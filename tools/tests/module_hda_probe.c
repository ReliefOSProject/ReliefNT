/* SPDX-License-Identifier: Apache-2.0 */
/* Native ReliefNT guest only: real HDA module, ALSA and VMA owner lifecycle. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sound/asound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief Fail an admitted private guest and retain diagnostics for its runner.
 * @param okay Required true condition. @param operation Stable diagnostic label.
 * PID1 pauses; a failed child exits nonzero. No host mutations occur.
 */
static void require(int okay,const char *operation)
{
    if(!okay) {
        printf("FAIL MODULE_HDA operation=%s errno=%d (%s)\n",operation,errno,strerror(errno));
        if(getpid()==1) for(;;) pause();
        _exit(1);
    }
}
/** @brief Require Linux nonblocking unload refusal while an owner is retained.
 * @param name Normalized module name. @param operation Stable diagnostic label.
 * Guest-only task context; the failed request must leave the module usable.
 */
static void busy(const char *name,const char *operation)
{
    errno=0;
    require(syscall(SYS_delete_module,name,O_NONBLOCK)==-1 && errno==EWOULDBLOCK,operation);
}
/** @brief Select one actually available CPU for cross-CPU final release.
 * @param cpu Available CPU index, or negative to retain the current affinity.
 */
static void choose_cpu(int cpu)
{
    if(cpu<0) return;
    cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);
    require(!sched_setaffinity(0,sizeof(set),&set),"set affinity");
}
/** @brief Play real PCM frames with the separately built native ALSA client.
 * Guest-only task context; the child must complete successfully before return.
 */
static void playback(void)
{
    pid_t child=fork();require(child>=0,"fork ALSA playback");
    if(!child) {
        execl("/qa-audio-player","audio-player","--mode","play","--backend","alsa","--frames","4096",(char *)NULL);
        _exit(127);
    }
    int status;require(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"ALSA playback child");
}
/** @brief Exercise actual target modprobe dependency resolution against this release.
 * Guest-only task context. The staged BusyBox and indexed module tree are real
 * production artifacts; the probe provides no substitute module dependency map.
 */
static void modprobe_hda(void)
{
    pid_t child=fork();require(child>=0,"fork native modprobe");
    if(!child){execl("/qa-busybox","busybox","modprobe","hda",(char *)NULL);_exit(127);}
    int status;require(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"native modprobe hard dependency");
}
/** @brief Test real sound-core/HDA ownership through playback and final VMA release.
 * @return 77 before mutations on a host or unowned root. The admitted private
 * PID1 pauses for its runner after success; all writes occur in the VM snapshot.
 */
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT") || getpid()!=1) {
        fputs("Guest-only HDA module probe: refusing host mutations\n",stderr);return 77;
    }
    static const char expected[]="ReliefNT owned boot runtime QA\n";
    char marker[sizeof(expected)];int owned=open("/ko-runtime-owned",O_RDONLY|O_CLOEXEC);
    ssize_t count=owned<0 ? -1 : read(owned,marker,sizeof(marker));if(owned>=0)close(owned);
    if(count!=sizeof(expected)-1 || memcmp(marker,expected,sizeof(expected)-1)) {
        fputs("Guest-only HDA module probe: refusing unowned root\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0) {dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);
    printf("MODULE_HDA_READY release=%s\n",identity.release);
    int absent=access("/qa-hda-absent",F_OK)==0;
    cpu_set_t available;CPU_ZERO(&available);require(!sched_getaffinity(0,sizeof(available),&available),"get affinity");
    int first=-1,second=-1;
    for(int i=0;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&available)){if(first<0)first=i;else{second=i;break;}}
    if(second<0)second=first;
    choose_cpu(first);
    int module=open("/hda.ko",O_RDONLY|O_CLOEXEC);require(module>=0,"open HDA module");
    int core=open("/snd.ko",O_RDONLY|O_CLOEXEC);
    if(core>=0) {
        errno=0;require(syscall(SYS_finit_module,module,"",0)==-1 && errno==ENOENT,
            "raw finit_module rejects missing sound dependency");
    }
    for(unsigned iteration=0;iteration<8;++iteration) {
        printf("MODULE_HDA_STAGE=load cycle=%u\n",iteration+1);
        if(core>=0) {
            require(!syscall(SYS_finit_module,core,"",0),"load native sound core");
            int timer=open("/dev/snd/timer",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            require(timer>=0,"open core-only ALSA timer");
            busy("snd","timer OFD prevents sound core unload");
            require(!close(timer),"close core-only ALSA timer");
            require(!syscall(SYS_delete_module,"snd",O_NONBLOCK),"idle core unload after timer close");
            modprobe_hda();
            busy("snd","HDA hard dependency prevents sound core unload");
        } else require(!syscall(SYS_finit_module,module,"",0),"load native HDA");
        puts("MODULE_HDA_STAGE=open-control");
        int control=open("/dev/snd/controlC0",O_RDWR|O_CLOEXEC);
        if(absent) {
            require(control<0 && (errno==ENOENT || errno==ENODEV),"no controller remains unbound");
            require(!syscall(SYS_delete_module,"hda",O_NONBLOCK),"unload unbound HDA");
            if(core>=0)require(!syscall(SYS_delete_module,"snd",O_NONBLOCK),"unload unbound sound core");
            printf("MODULE_HDA_CYCLE=%u mode=no-device\n",iteration+1);continue;
        }
        require(control>=0,"open ALSA control");
        puts("MODULE_HDA_STAGE=control-info");
        struct snd_ctl_card_info info={0};require(!ioctl(control,SNDRV_CTL_IOCTL_CARD_INFO,&info),"ALSA card info");
        puts("MODULE_HDA_STAGE=control-busy");
        busy("hda","control OFD prevents HDA unload");
        puts("MODULE_HDA_STAGE=playback");
        playback();
        puts("MODULE_HDA_STAGE=pcm-mapping");
        int pcm=open("/dev/snd/pcmC0D0p",O_RDWR|O_CLOEXEC);require(pcm>=0,"open ALSA PCM");
        void *status=mmap(NULL,4096,PROT_READ,MAP_SHARED,pcm,SNDRV_PCM_MMAP_OFFSET_STATUS);
        require(status!=MAP_FAILED,"map PCM status");
        close(pcm);close(control);
        busy("hda","mapping after close prevents HDA unload");
        if(core>=0)busy("snd","mapping retains original sound core after FD close");
        int barrier[2];require(!pipe(barrier),"VMA barrier");
        pid_t child=fork();require(child>=0,"fork VMA holder");
        if(!child) {
            close(barrier[1]);char go;require(read(barrier[0],&go,1)==1,"VMA barrier wait");close(barrier[0]);
            choose_cpu(second);require(!munmap(status,4096),"child final VMA release");
            require(!syscall(SYS_delete_module,"hda",O_NONBLOCK),"HDA teardown after final VMA on other CPU");
            if(core>=0)require(!syscall(SYS_delete_module,"snd",O_NONBLOCK),"sound core teardown after controller and final VMA");
            _exit(0);
        }
        close(barrier[0]);require(!munmap(status,4096),"parent VMA release");
        busy("hda","forked VMA prevents HDA unload");
        require(write(barrier[1],"x",1)==1,"wake final VMA holder");close(barrier[1]);
        int result;require(waitpid(child,&result,0)==child && WIFEXITED(result) && !WEXITSTATUS(result),"VMA/unload child result");
        printf("MODULE_HDA_CYCLE=%u mode=alsa-dma-ofd-vma\n",iteration+1);
    }
    close(module);
    if(core>=0)close(core);
    printf("PASS MODULE_HDA eight native cycles, %s\n",absent ? "absent-controller registration" :
           "ALSA DMA playback, busy OFD/VMA, fork mapping lifetime, safe controller teardown");
    if(getpid()==1) for(;;) pause();
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
/* Native ReliefNT only: legacy audio owner, PCI DMA and safe module teardown. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static void require(int okay,const char *operation)
{
    if(okay) return;
    printf("FAIL MODULE_LEGACY_AUDIO operation=%s errno=%d (%s)\n",operation,errno,strerror(errno));
    if(getpid()==1) for(;;) pause();
    _exit(1);
}
static void choose_cpu(int cpu)
{
    if(cpu<0) return;
    cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);
    require(!sched_setaffinity(0,sizeof(set),&set),"set affinity");
}
static void playback(void)
{
    pid_t child=fork();require(child>=0,"fork OSS playback");
    if(!child) {
        execl("/qa-audio-player","audio-player","--mode","play","--backend","oss",
              "--frames","4096",(char *)NULL);
        _exit(127);
    }
    int status;
    require(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),
            "OSS playback child");
}
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT")) {
        fputs("Guest-only audio module probe: refusing host module operations\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0) {dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);
    char name[16]={0},path[32];
    FILE *selection=fopen("/qa-legacy-driver","r");
    require(selection && fscanf(selection,"%15s",name)==1,"driver selection");fclose(selection);
    require(!strcmp(name,"ac97") || !strcmp(name,"es1371"),"supported legacy driver");
    snprintf(path,sizeof(path),"/%s.ko",name);
    printf("MODULE_LEGACY_AUDIO_READY driver=%s release=%s\n",name,identity.release);
    int absent=access("/qa-audio-absent",F_OK)==0;
    cpu_set_t available;CPU_ZERO(&available);
    require(!sched_getaffinity(0,sizeof(available),&available),"get affinity");
    int first=-1,second=-1;
    for(int i=0;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&available)) {
        if(first<0)first=i;else{second=i;break;}
    }
    if(second<0)second=first;
    choose_cpu(first);
    int module=open(path,O_RDONLY|O_CLOEXEC);require(module>=0,"open native module");
    for(unsigned iteration=0;iteration<8;++iteration) {
        int barrier[2];require(!pipe(barrier),"unload barrier");
        pid_t child=fork();require(child>=0,"fork before module load");
        if(!child) {
            close(barrier[1]);char ready;
            require(read(barrier[0],&ready,1)==1,"wait unload barrier");close(barrier[0]);
            choose_cpu(second);
            require(!syscall(SYS_delete_module,name,O_NONBLOCK),"safe unload on preexisting CR3");
            _exit(0);
        }
        close(barrier[0]);
        require(!syscall(SYS_finit_module,module,"",0),"load native audio module");
        int dsp=open("/dev/dsp",O_WRONLY|O_CLOEXEC);
        if(absent) require(dsp<0 && (errno==ENODEV || errno==ENOENT),"absent hardware remains unbound");
        else {
            require(dsp>=0,"open legacy OSS provider");
            errno=0;
            require(syscall(SYS_delete_module,name,O_NONBLOCK)==-1 && errno==EWOULDBLOCK,
                    "OSS description prevents module unload");
            close(dsp);
            playback();
        }
        require(write(barrier[1],"x",1)==1,"wake unload child");close(barrier[1]);
        int status;
        require(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),
                "unload child result");
        printf("MODULE_LEGACY_AUDIO_CYCLE=%u driver=%s mode=%s\n",iteration+1,name,
               absent?"no-device":"OSS-DMA-owner");
    }
    close(module);
    printf("PASS MODULE_LEGACY_AUDIO eight %s cycles, %s\n",name,
           absent?"absent-device registration":"OSS playback, busy owner, PCI DMA and teardown on preexisting CR3");
    if(getpid()==1)for(;;)pause();
    return 0;
}

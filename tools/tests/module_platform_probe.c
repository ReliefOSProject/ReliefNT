/* SPDX-License-Identifier: Apache-2.0 */
/* Native ReliefNT only: modular serial and mouse providers, permanent TTY/input core. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static void require(int okay,const char *operation)
{
    if(okay)return;
    printf("FAIL MODULE_PLATFORM operation=%s errno=%d (%s)\n",operation,errno,strerror(errno));
    if(getpid()==1)for(;;)pause();
    _exit(1);
}
static void choose_cpu(int cpu)
{
    if(cpu<0)return;
    cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);
    require(!sched_setaffinity(0,sizeof(set),&set),"set affinity");
}
static void input_event_check(unsigned cycle)
{
    int input=open("/dev/input/event1",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    require(input>=0,"open mouse evdev");
    char name[128]={0};require(ioctl(input,EVIOCGNAME(sizeof(name)),name)>=0 && name[0],"evdev identity");
    struct input_event events[32];
    while(read(input,events,sizeof(events))>0){}
    printf("MODULE_PLATFORM_INPUT_READY cycle=%u\n",cycle);
    int motion=0;
    for(unsigned tries=0;tries<10 && !motion;++tries) {
        struct pollfd watch={.fd=input,.events=POLLIN};
        int result=poll(&watch,1,1000);require(result>=0,"wait native pointer event");
        if(!result)continue;
        ssize_t bytes=read(input,events,sizeof(events));
        require(bytes>=0 || errno==EAGAIN,"read native pointer events");
        for(ssize_t n=0;n>=(ssize_t)0 && n<bytes/(ssize_t)sizeof(*events);++n)
            if(events[n].type==EV_REL || events[n].type==EV_ABS)motion=1;
    }
    require(motion,"mouse module produced real QEMU input motion");close(input);
}
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT")) {
        fputs("Guest-only platform module probe: refusing host module operations\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0) {dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);
    printf("MODULE_PLATFORM_READY release=%s\n",identity.release);
    cpu_set_t available;CPU_ZERO(&available);
    require(!sched_getaffinity(0,sizeof(available),&available),"get affinity");
    int first=-1,second=-1;
    for(int i=0;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&available)) {
        if(first<0)first=i;else{second=i;break;}
    }
    if(second<0)second=first;
    choose_cpu(first);
    int mouse=open("/mouse.ko",O_RDONLY|O_CLOEXEC),uart=open("/serial.ko",O_RDONLY|O_CLOEXEC);
    require(mouse>=0 && uart>=0,"open platform modules");
    for(unsigned cycle=1;cycle<=8;++cycle) {
        int barrier[2];require(!pipe(barrier),"unload barrier");
        pid_t child=fork();require(child>=0,"fork before module load");
        if(!child) {
            close(barrier[1]);char ready;
            require(read(barrier[0],&ready,1)==1,"wait unload barrier");close(barrier[0]);
            choose_cpu(second);
            require(!syscall(SYS_delete_module,"mouse",O_NONBLOCK),"unload mouse on preexisting CR3");
            require(!syscall(SYS_delete_module,"serial",O_NONBLOCK),"unload serial on preexisting CR3");
            _exit(0);
        }
        close(barrier[0]);
        require(!syscall(SYS_finit_module,uart,"",0),"load serial provider");
        printf("MODULE_PLATFORM_SERIAL_OUT cycle=%u\n",cycle);
        require(!syscall(SYS_finit_module,mouse,"",0),"load mouse provider");
        input_event_check(cycle);
        require(write(barrier[1],"x",1)==1,"wake unload child");close(barrier[1]);
        int status;
        require(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"unload child result");
        printf("MODULE_PLATFORM_CYCLE=%u permanent-TTY-after-unload\n",cycle);
    }
    close(mouse);close(uart);
    puts("PASS MODULE_PLATFORM eight serial+mouse cycles, native input motion, preexisting CR3 and permanent TTY fallback");
    if(getpid()==1)for(;;)pause();
    return 0;
}

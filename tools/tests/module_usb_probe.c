/* SPDX-License-Identifier: Apache-2.0 */
/* Native ReliefNT only: UHCI DMA, USB HID, held-key removal and module lifetime. */
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
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static void require(int okay,const char *operation)
{
    if(okay)return;
    printf("FAIL MODULE_USB operation=%s errno=%d (%s)\n",operation,errno,strerror(errno));
    if(getpid()==1)for(;;)pause();
    _exit(1);
}
static void choose_cpu(int cpu)
{
    if(cpu<0)return;
    cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);
    require(!sched_setaffinity(0,sizeof(set),&set),"set affinity");
}
static void drain(int fd)
{
    struct input_event events[32];
    while(read(fd,events,sizeof(events))>0){}
    require(errno==EAGAIN,"drain nonblocking evdev");
}
static void input_check(int keyboard,int mouse,unsigned key,int release_only)
{
    int key_seen=0,motion=release_only;
    for(unsigned tries=0;tries<10 && (!key_seen || !motion);++tries) {
        struct pollfd watch[2]={{.fd=keyboard,.events=POLLIN},
                               {.fd=release_only ? -1 : mouse,.events=POLLIN}};
        int ready=poll(watch,2,1000);require(ready>=0,"poll native USB events");
        for(unsigned device=0;device<2;++device) {
            if(!(watch[device].revents&POLLIN))continue;
            struct input_event events[32];
            ssize_t bytes=read(watch[device].fd,events,sizeof(events));
            require(bytes>=0 || errno==EAGAIN,"read USB evdev");
            if(bytes<0)continue;
            require(bytes%(ssize_t)sizeof(*events)==0,"whole evdev records");
            for(size_t n=0;n<(size_t)bytes/sizeof(*events);++n) {
                if(!device && events[n].type==EV_KEY && events[n].code==key &&
                   events[n].value==(release_only ? 0 : 1))key_seen=1;
                if(device && (events[n].type==EV_REL || events[n].type==EV_ABS))motion=1;
            }
        }
    }
    require(key_seen && motion,release_only ? "held USB key released by driver teardown" :
            "real UHCI keyboard and mouse input");
}
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT")) {
        fputs("Guest-only USB module probe: refusing host module operations\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0) {dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);
    printf("MODULE_USB_READY release=%s\n",identity.release);
    int absent=access("/qa-usb-absent",F_OK)==0;
    cpu_set_t available;CPU_ZERO(&available);
    require(!sched_getaffinity(0,sizeof(available),&available),"get affinity");
    int first=-1,second=-1;
    for(int i=0;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&available)) {
        if(first<0)first=i;else{second=i;break;}
    }
    if(second<0)second=first;
    choose_cpu(first);
    int module=open("/uhci.ko",O_RDONLY|O_CLOEXEC);require(module>=0,"open UHCI module");
    /* Linux input-event-codes.h KEY_A through KEY_H in alphabetic order. */
    const unsigned keys[]={30,48,46,32,18,33,34,35};
    for(unsigned cycle=1;cycle<=8;++cycle) {
        int barrier[2];require(!pipe(barrier),"unload barrier");
        pid_t child=fork();require(child>=0,"fork before driver load");
        if(!child) {
            close(barrier[1]);char ready;
            require(read(barrier[0],&ready,1)==1,"wait unload barrier");close(barrier[0]);
            choose_cpu(second);
            require(!syscall(SYS_delete_module,"uhci",O_NONBLOCK),"unload UHCI on preexisting CR3");
            _exit(0);
        }
        close(barrier[0]);
        require(!syscall(SYS_finit_module,module,"",0),"load native UHCI");
        int keyboard=-1,mouse=-1;
        if(!absent) {
            keyboard=open("/dev/input/event0",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            mouse=open("/dev/input/event1",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            require(keyboard>=0 && mouse>=0,"open permanent evdev endpoints");
            drain(keyboard);drain(mouse);
            printf("MODULE_USB_INPUT_READY cycle=%u\n",cycle);
            input_check(keyboard,mouse,keys[cycle-1],0);
        }
        require(write(barrier[1],"x",1)==1,"wake unload child");close(barrier[1]);
        int status;
        require(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"unload child result");
        if(!absent) {
            input_check(keyboard,mouse,keys[cycle-1],1);
            close(keyboard);close(mouse);
        }
        printf("MODULE_USB_CYCLE=%u mode=%s\n",cycle,absent ? "no-device" : "HID-DMA-key-release");
    }
    close(module);
    printf("PASS MODULE_USB eight cycles, %s\n",absent ? "absent-controller registration" :
           "real UHCI keyboard/mouse input, held-key release, explicit DMA and cross-CPU safe teardown");
    if(getpid()==1)for(;;)pause();
    return 0;
}

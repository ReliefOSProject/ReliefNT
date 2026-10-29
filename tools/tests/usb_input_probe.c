/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <linux/input.h>

/** @brief Observe relative USB mouse input using the Linux evdev ABI. */
static int mouse(bool exact)
{
    /* This diagnostic observes raw physical input, rather than inheriting
     * the launching terminal's graphical-VT filter. Stdio is a pipe/serial
     * stream in the integration harness, with no explicit VT descriptor. */
    if(setsid()<0){perror("mouse setsid");return 1;}
    int fd=open("/dev/input/event1",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){perror("mouse open");return 1;}
    if(ioctl(fd,EVIOCGRAB,1)<0){perror("mouse grab");close(fd);return 1;}
    printf("USB_MOUSE_READY\n");fflush(stdout);
    bool x=false,y=false,down=false,up=false;
    struct timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);
    for(;;) {
        clock_gettime(CLOCK_MONOTONIC,&now);
        if(now.tv_sec-start.tv_sec>=12)break;
        struct pollfd poller={.fd=fd,.events=POLLIN};
        int ready=poll(&poller,1,250);
        if(ready<0 && errno!=EINTR){perror("mouse poll");break;}
        if(ready<=0)continue;
        struct input_event events[16];ssize_t bytes=read(fd,events,sizeof(events));
        if(bytes<0 && (errno==EINTR || errno==EAGAIN))continue;
        if(bytes<=0 || bytes%(ssize_t)sizeof(*events)){perror("mouse read");break;}
        for(ssize_t i=0;i<bytes/(ssize_t)sizeof(*events);++i) {
            const struct input_event *event=&events[i];
            printf("EV %u %u %d\n",event->type,event->code,event->value);
            if(event->type==EV_REL && event->code==REL_X &&
               (exact ? event->value==15 : event->value>0))x=true;
            if(event->type==EV_REL && event->code==REL_Y &&
               (exact ? event->value==9 : event->value>0))y=true;
            if(event->type==EV_KEY && event->code==BTN_LEFT && event->value==1)down=true;
            if(event->type==EV_KEY && event->code==BTN_LEFT && !event->value)up=true;
        }
        if(x && y && down && up){close(fd);puts("USB_MOUSE_OK");return 0;}
    }
    close(fd);printf("USB_MOUSE_FAILED x=%d y=%d down=%d up=%d\n",x,y,down,up);return 1;
}

/** @brief Verify physical write protection through block I/O and Linux mount ABI. */
static int readonly_block(const char *device,const char *target)
{
    int fd=open(device,O_RDWR|O_CLOEXEC);
    if(fd<0) {
        if(errno!=EROFS){perror("readonly block open");return 1;}
    } else {
        unsigned char sector[512]={0};errno=0;
        ssize_t bytes=write(fd,sector,sizeof(sector));int error=errno;close(fd);
        if(bytes>=0 || error!=EROFS) {
            printf("USB_RAW_RO_FAILED bytes=%ld errno=%d\n",(long)bytes,error);return 1;
        }
    }
    puts("USB_RAW_WRITE_EROFS");
    errno=0;
    if(!mount(device,target,"vfat",0,NULL)) {
        (void)umount(target);puts("USB_RW_MOUNT_WRONGLY_ACCEPTED");return 1;
    }
    if(errno!=EROFS){perror("rw mount expected EROFS");return 1;}
    if(mount(device,target,"vfat",MS_RDONLY,NULL)){perror("ro mount");return 1;}
    errno=0;
    int changed=mount(NULL,target,NULL,MS_REMOUNT,NULL),error=errno;
    struct statvfs status;int inspected=statvfs(target,&status);
    if(!changed || error!=EROFS || inspected || !(status.f_flag&ST_RDONLY)) {
        printf("USB_REMOUNT_RO_FAILED result=%d errno=%d stat=%d\n",changed,error,inspected);
        (void)umount(target);return 1;
    }
    puts("USB_RO_MOUNT_REMOUNT_OK");return 0;
}

/** @brief Keep an old block OFD across physical removal and replacement. */
static int old_block(const char *path)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0){perror("block open");return 1;}
    puts("USB_OLD_FD_READY");fflush(stdout);
    struct timespec delay={.tv_sec=8};while(nanosleep(&delay,&delay) && errno==EINTR){}
    unsigned char bytes[512];errno=0;ssize_t result=read(fd,bytes,sizeof(bytes));int error=errno;
    close(fd);
    if(result<0 && error==ENODEV){puts("USB_OLD_FD_ENODEV");return 0;}
    printf("USB_OLD_FD_FAILED bytes=%ld errno=%d\n",(long)result,error);return 1;
}

/** @brief Run a native input/removal integration check, with exact errno evidence. */
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"mouse"))return mouse(true);
    /* RFB coordinates may be scaled by the virtual desktop. Still require
     * observed positive motion on both relative axes plus button down/up. */
    if(argc==2 && !strcmp(argv[1],"mouse-rfb"))return mouse(false);
    if(argc==3 && !strcmp(argv[1],"block"))return old_block(argv[2]);
    if(argc==4 && !strcmp(argv[1],"readonly"))return readonly_block(argv[2],argv[3]);
    fprintf(stderr,"usage: usb-input-probe mouse | mouse-rfb | block DEVICE | readonly DEVICE MOUNTPOINT\n");return 2;
}

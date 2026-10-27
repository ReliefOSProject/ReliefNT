/* SPDX-License-Identifier: Apache-2.0 */
/* Guest-only native module + real PCI DMA/network lifecycle probe. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static void require(int okay,const char *operation)
{
    if(!okay) {
        printf("FAIL MODULE_E1000 operation=%s errno=%d (%s)\n",operation,errno,strerror(errno));
        if(getpid()==1) for(;;) pause();
        _exit(1);
    }
}
static void choose_cpu(int cpu)
{
    if(cpu<0) return;
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu,&set);
    require(!sched_setaffinity(0,sizeof(set),&set),"set affinity");
}
static void configure(int control)
{
    struct ifreq request={0}; strcpy(request.ifr_name,"eth0");
    struct sockaddr_in *address=(void *)&request.ifr_addr;
    address->sin_family=AF_INET; require(inet_pton(AF_INET,"10.0.2.15",&address->sin_addr)==1,"parse local address");
    require(!ioctl(control,SIOCSIFADDR,&request),"configure IPv4");
    address->sin_family=AF_INET; require(inet_pton(AF_INET,"255.255.255.0",&address->sin_addr)==1,"parse netmask");
    require(!ioctl(control,SIOCSIFNETMASK,&request),"configure netmask");
    request.ifr_flags=IFF_UP; require(!ioctl(control,SIOCSIFFLAGS,&request),"bring up interface");
}
static void check_echo(unsigned port,unsigned iteration)
{
    int fd=socket(AF_INET,SOCK_STREAM,0); require(fd>=0,"create TCP socket");
    struct timeval timeout={10,0};
    require(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)),"receive timeout");
    require(!setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)),"send timeout");
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    require(inet_pton(AF_INET,"10.0.2.2",&address.sin_addr)==1,"parse echo address");
    require(!connect(fd,(void *)&address,sizeof(address)),"connect TCP echo");
    unsigned char bytes[32768],response[32768];
    for(unsigned i=0;i<sizeof(bytes);++i) bytes[i]=(unsigned char)(i*37u+iteration);
    size_t sent=0,received=0;
    while(sent<sizeof(bytes)) { ssize_t n=send(fd,bytes+sent,sizeof(bytes)-sent,0); require(n>0,"send echo data"); sent+=(size_t)n; }
    while(received<sizeof(response)) { ssize_t n=recv(fd,response+received,sizeof(response)-received,0); require(n>0,"receive echo data"); received+=(size_t)n; }
    require(!memcmp(bytes,response,sizeof(bytes)),"TCP data checksum");
    close(fd);
}
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT")) {
        fputs("Guest-only e1000 module probe: refusing host module operations\n",stderr); return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0) { dup2(serial,1); dup2(serial,2); if(serial>2) close(serial); }
    setvbuf(stdout,NULL,_IONBF,0);
    printf("MODULE_E1000_READY release=%s\n",identity.release);
    int absent=access("/qa-no-network",F_OK)==0;
    unsigned port=0;
    if(!absent) { FILE *file=fopen("/qa-echo-port","r"); require(file && fscanf(file,"%u",&port)==1 && port && port<65536,"echo port"); fclose(file); }
    cpu_set_t available; CPU_ZERO(&available); require(!sched_getaffinity(0,sizeof(available),&available),"get affinity");
    int first=-1,second=-1;
    for(int i=0;i<CPU_SETSIZE;++i) if(CPU_ISSET(i,&available)) { if(first<0) first=i; else {second=i;break;} }
    if(second<0) second=first;
    choose_cpu(first);
    int module=open("/e1000.ko",O_RDONLY); require(module>=0,"open e1000 module");
    int control=socket(AF_INET,SOCK_DGRAM,0); require(control>=0,"create ioctl socket");
    struct ifreq request={0}; strcpy(request.ifr_name,"eth0");
    errno=0; require(ioctl(control,SIOCGIFFLAGS,&request)==-1 && errno==ENODEV,"e1000 absent before load");
    for(unsigned iteration=0;iteration<8;++iteration) {
        int barrier[2]; require(!pipe(barrier),"create unload barrier");
        pid_t child=fork(); require(child>=0,"fork before driver load");
        if(!child) {
            close(barrier[1]); char ready;
            require(read(barrier[0],&ready,1)==1,"unload barrier"); close(barrier[0]);
            choose_cpu(second);
            require(!syscall(SYS_delete_module,"e1000",O_NONBLOCK),"unload e1000 on preexisting CR3"); _exit(0);
        }
        close(barrier[0]);
        require(!syscall(SYS_finit_module,module,"",0),"load native e1000");
        int result=ioctl(control,SIOCGIFFLAGS,&request);
        if(absent) require(result==-1 && errno==ENODEV,"no device remains unbound");
        else { require(!result,"e1000 ready after load"); configure(control); check_echo(port,iteration); }
        errno=0; require(syscall(SYS_finit_module,module,"",0)==-1 && errno==EEXIST,"duplicate module rejected");
        require(write(barrier[1],"x",1)==1,"wake unload child"); close(barrier[1]);
        int status=0; require(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"unload child result");
        errno=0; require(ioctl(control,SIOCGIFFLAGS,&request)==-1 && errno==ENODEV,"interface absent after unload");
        printf("MODULE_E1000_CYCLE=%u mode=%s\n",iteration+1,absent ? "no-device" : "tcp-dma");
    }
    close(module); close(control);
    printf("PASS MODULE_E1000 eight native driver cycles, existing CR3, %s\n",absent ? "absent-device registration" : "PCI DMA, TCP data checksum, reset and callback teardown");
    if(getpid()==1) for(;;) pause();
    return 0;
}

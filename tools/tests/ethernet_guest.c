/* SPDX-License-Identifier: Apache-2.0 */
/* Native Linux/POSIX socket probe; consumed by ReliefOS's VM orchestration. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static void require(int good, const char *operation)
{
    if (!good) {
        fprintf(stderr,"ETHERNET_FAIL operation=%s errno=%d (%s)\n",operation,errno,strerror(errno));
        exit(1);
    }
}
static int connect_peer(int family, int type, unsigned port)
{
    int fd=socket(family,type,0);
    require(fd>=0,"socket");
    struct timeval timeout={10,0};
    require(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)),"receive timeout");
    /* UDP admission is immediate; only stream sends need a blocking timeout. */
    if (type==SOCK_STREAM)
        require(!setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)),"send timeout");
    struct sockaddr_storage address={0};
    socklen_t size;
    if (family==AF_INET) {
        struct sockaddr_in *a=(void *)&address;
        a->sin_family=AF_INET; a->sin_port=htons((uint16_t)port); size=sizeof(*a);
        require(inet_pton(AF_INET,"10.0.2.2",&a->sin_addr)==1,"parse IPv4");
    } else {
        struct sockaddr_in6 *a=(void *)&address;
        a->sin6_family=AF_INET6; a->sin6_port=htons((uint16_t)port); size=sizeof(*a);
        require(inet_pton(AF_INET6,"fec0::2",&a->sin6_addr)==1,"parse IPv6");
    }
    require(!connect(fd,(void *)&address,size),"connect echo peer");
    return fd;
}
static void transfer(int fd, void *bytes, size_t size, int sending)
{
    unsigned char *at=bytes;
    while (size) {
        ssize_t count=sending?send(fd,at,size,0):recv(fd,at,size,0);
        if (count<0 && errno==EINTR) continue;
        require(count>0,sending?"TCP send":"TCP receive");
        at+=count; size-=(size_t)count;
    }
}
static void check_tcp(int family, unsigned port, unsigned stream)
{
    int fd=connect_peer(family,SOCK_STREAM,port);
    unsigned char expected[8192], actual[8192];
    for (unsigned round=0;round<64;++round) {
        for (unsigned i=0;i<sizeof(expected);++i)
            expected[i]=(unsigned char)(i*37+round*19+stream*113);
        transfer(fd,expected,sizeof(expected),1);
        transfer(fd,actual,sizeof(actual),0);
        require(!memcmp(expected,actual,sizeof(expected)),"TCP payload integrity");
    }
    require(!close(fd),"TCP close");
}
static void check_udp(int family, unsigned port)
{
    int fd=connect_peer(family,SOCK_DGRAM,port);
    unsigned char expected[333], actual[334];
    for (unsigned n=0;n<32;++n) {
        for (unsigned i=0;i<sizeof(expected);++i) expected[i]=(unsigned char)(n*71+i*13);
        require(send(fd,expected,sizeof(expected),0)==sizeof(expected),"UDP send");
        require(recv(fd,actual,sizeof(actual),0)==sizeof(expected),"UDP receive length");
        require(!memcmp(expected,actual,sizeof(expected)),"UDP payload integrity");
    }
    require(!close(fd),"UDP close");
}
int main(int argc, char **argv)
{
    require(argc==2,"echo port argument");
    char *end;
    unsigned long number=strtoul(argv[1],&end,10);
    require(*argv[1] && !*end && number>0 && number<=65535,"valid echo port");
    unsigned port=(unsigned)number;
    for (unsigned v=0;v<2;++v) {
        int family=v?AF_INET6:AF_INET;
        pid_t children[2];
        fflush(NULL);
        for (unsigned i=0;i<2;++i) {
            children[i]=fork(); require(children[i]>=0,"fork concurrent TCP stream");
            if (!children[i]) { check_tcp(family,port,i); _exit(0); }
        }
        for (unsigned i=0;i<2;++i) {
            int status;
            require(waitpid(children[i],&status,0)==children[i] && WIFEXITED(status) &&
                    WEXITSTATUS(status)==0,"concurrent TCP stream result");
        }
        check_udp(family,port);
        printf("ETHERNET_PASS family=%u tcp_streams=2 tcp_bytes=1048576 udp_datagrams=32\n",v?6:4);
        fflush(stdout);
    }
    return 0;
}

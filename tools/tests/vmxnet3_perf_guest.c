/* SPDX-License-Identifier: Apache-2.0 */
/* Linux/POSIX socket probe: verified TCP echo throughput and UDP round trips. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

/** @brief Abort a failed probe with the failing operation and errno. */
static void fail(const char *operation) { perror(operation); exit(1); }
/** @brief Read nanoseconds from a supported POSIX clock. */
static uint64_t now(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock,&ts)) fail("clock_gettime");
    return (uint64_t)ts.tv_sec*1000000000u+(uint64_t)ts.tv_nsec;
}
/** @brief Read POSIX per-process user/system CPU time without optional CPU clocks. */
static uint64_t process_cpu(void)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF,&usage)) fail("getrusage");
    return ((uint64_t)usage.ru_utime.tv_sec+(uint64_t)usage.ru_stime.tv_sec)*1000000000u+
        ((uint64_t)usage.ru_utime.tv_usec+(uint64_t)usage.ru_stime.tv_usec)*1000u;
}
/** @brief Read Linux aggregate user/system/idle accounting in USER_HZ ticks. */
static void cpu(uint64_t *total,uint64_t *busy)
{
    unsigned long long user,nice,system,idle,wait,irq,soft,steal;
    FILE *file=fopen("/proc/stat","r");
    if (!file) fail("/proc/stat");
    int count=fscanf(file,"cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &user,&nice,&system,&idle,&wait,&irq,&soft,&steal);
    fclose(file);
    if (count!=8) { errno=EIO; fail("parse /proc/stat"); }
    *busy=user+nice+system+irq+soft+steal; *total=*busy+idle+wait;
}
/** @brief Open a connected host echo socket on one Linux interface. */
static int connect_peer(const char *interface,const char *host,unsigned port,int type)
{
    int fd=socket(AF_INET,type,0);
    if (fd<0) fail("socket");
    if (setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,interface,strlen(interface)+1)) fail("SO_BINDTODEVICE");
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    if (inet_pton(AF_INET,host,&address.sin_addr)!=1) { errno=EINVAL; fail("inet_pton"); }
    if (connect(fd,(void *)&address,sizeof(address))) fail("connect");
    return fd;
}
/** @brief Compare unsigned RTT samples for percentile selection. */
static int compare(const void *a,const void *b)
{ uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y); }
/** @brief Verify a full duplex TCP stream, then measure 128 UDP echo latencies.
 * @return Zero only after every echoed byte and datagram has been checked.
 */
int main(int argc,char **argv)
{
    if (argc!=5) { fprintf(stderr,"usage: %s interface peer port bytes\n",argv[0]);return 2; }
    unsigned port=(unsigned)strtoul(argv[3],NULL,10);
    size_t bytes=(size_t)strtoull(argv[4],NULL,10);
    if (!port || port>65535 || !bytes || bytes>1024u*1024u*1024u) return 2;
    int fd=connect_peer(argv[1],argv[2],port,SOCK_STREAM);
    int flags=fcntl(fd,F_GETFL);
    if (flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) fail("nonblocking socket");
    uint8_t tx[8192],rx[8192];
    uint64_t total0,busy0,total1,busy1;
    cpu(&total0,&busy0);
    uint64_t process0=process_cpu(), start=now(CLOCK_MONOTONIC);
    size_t sent=0,received=0;
    while (received<bytes) {
        struct pollfd p={.fd=fd,.events=POLLIN|(sent<bytes?POLLOUT:0)};
        int ready=poll(&p,1,5000);
        if (ready<0 && errno==EINTR) continue;
        if (ready<=0 || (p.revents&(POLLERR|POLLNVAL))) { errno=ETIMEDOUT;fail("TCP poll"); }
        if (p.revents&POLLOUT) {
            size_t n=bytes-sent; if (n>sizeof(tx)) n=sizeof(tx);
            for (size_t i=0;i<n;++i) tx[i]=(uint8_t)((sent+i)*37u+11u);
            ssize_t result=send(fd,tx,n,MSG_NOSIGNAL);
            if (result>0) sent+=(size_t)result;
            else if (result<0 && errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK) fail("TCP send");
        }
        if (p.revents&(POLLIN|POLLHUP)) {
            ssize_t n=recv(fd,rx,sizeof(rx),0);
            if (!n) { errno=EPIPE;fail("early TCP EOF"); }
            if (n<0) { if (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK) continue;fail("TCP receive"); }
            if ((size_t)n>bytes-received) { errno=EIO;fail("extra TCP data"); }
            for (ssize_t i=0;i<n;++i)
                if (rx[i]!=(uint8_t)((received+(size_t)i)*37u+11u)) { errno=EIO;fail("TCP integrity"); }
            received+=(size_t)n;
        }
        if (now(CLOCK_MONOTONIC)-start>120000000000ull) { errno=ETIMEDOUT;fail("TCP deadline"); }
    }
    uint64_t elapsed=now(CLOCK_MONOTONIC)-start, process=process_cpu()-process0;
    cpu(&total1,&busy1); close(fd);
    fd=connect_peer(argv[1],argv[2],port,SOCK_DGRAM);
    uint64_t samples[128];
    for (unsigned i=0;i<128;++i) {
        memset(tx,(int)i,64); uint64_t before=now(CLOCK_MONOTONIC);
        if (send(fd,tx,64,0)!=64) fail("UDP send");
        struct pollfd p={.fd=fd,.events=POLLIN};
        if (poll(&p,1,3000)!=1 || recv(fd,rx,sizeof(rx),0)!=64 || memcmp(tx,rx,64)) {
            errno=EIO;fail("UDP echo");
        }
        samples[i]=now(CLOCK_MONOTONIC)-before;
    }
    close(fd); qsort(samples,128,sizeof(samples[0]),compare);
    printf("VMXNET3_PERF {\"bytes\":%zu,\"elapsed_ns\":%llu,\"process_cpu_ns\":%llu,"
        "\"cpu_total_ticks\":%llu,\"cpu_busy_ticks\":%llu,\"udp_p50_ns\":%llu,\"udp_p95_ns\":%llu}\n",
        bytes,(unsigned long long)elapsed,(unsigned long long)process,
        (unsigned long long)(total1-total0),(unsigned long long)(busy1-busy0),
        (unsigned long long)samples[63],(unsigned long long)samples[121]);
    return 0;
}

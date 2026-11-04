/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static unsigned failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr,"[pipe-parallel] FAIL line=%d errno=%d: %s\n",__LINE__,errno,#condition);++failures; \
} } while (0)

/** @brief Read the entire bounded record, tolerating ordinary short reads. */
static int receive(int fd, void *buffer, size_t count)
{
    size_t offset=0;
    while(offset<count) {
        ssize_t done=read(fd,(char *)buffer+offset,count-offset);
        if(done<0 && errno==EINTR)continue;
        if(done<=0)return -1;
        offset+=(size_t)done;
    }
    return 0;
}

/** @brief Require one successfully reaped probe child. */
static void joined(pid_t child)
{
    int status=0;pid_t waited;
    do { waited=waitpid(child,&status,0); } while(waited<0 && errno==EINTR);
    CHECK(waited==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
}

/** @brief Compete over a shared pipe using indivisible POSIX-sized records. */
static void records(size_t size)
{
    int pair[2];if(pipe(pair)) { CHECK(0);return; }
    enum { ROUNDS=512 };pid_t children[2];
    for(unsigned writer=0;writer<2;++writer) {
        children[writer]=fork();CHECK(children[writer]>=0);
        if(!children[writer]) {
            close(pair[0]);unsigned char record[4096];
            for(unsigned sequence=0;sequence<ROUNDS;++sequence) {
                memset(record,(int)(writer+1),size);
                uint32_t header[]={writer+1,sequence};memcpy(record,header,sizeof(header));
                ssize_t done;
                do { done=write(pair[1],record,size); } while(done<0 && errno==EINTR);
                if(done!=(ssize_t)size)_exit(1);
            }
            close(pair[1]);_exit(0);
        }
    }
    close(pair[1]);unsigned next[2]={0};unsigned char record[4096];
    for(unsigned i=0;i<2*ROUNDS;++i) {
        if(receive(pair[0],record,size)) { CHECK(0);break; }
        uint32_t header[2];memcpy(header,record,sizeof(header));
        if(header[0]<1 || header[0]>2) { CHECK(0);break; }
        unsigned writer=header[0]-1;CHECK(header[1]==next[writer]++);
        for(size_t j=sizeof(header);j<size;++j)if(record[j]!=writer+1) { CHECK(0);break; }
    }
    CHECK(next[0]==ROUNDS && next[1]==ROUNDS);CHECK(read(pair[0],record,1)==0);
    close(pair[0]);for(unsigned i=0;i<2;++i)if(children[i]>0)joined(children[i]);
    printf("[pipe-parallel] records size=%zu writers=2 each=%u\n",size,ROUNDS);
}

/** @brief Receive into untouched anonymous pages, then private COW backing. */
static void destinations(void)
{
    unsigned char *bytes=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(bytes==MAP_FAILED) { CHECK(0);return; }
    int data[2],control[2];CHECK(pipe(data)==0 && pipe(control)==0);
    CHECK(write(data[1],"lazy",4)==4 && read(data[0],bytes,4)==4 && !memcmp(bytes,"lazy",4));
    memset(bytes,0x3a,8192);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        close(control[1]);char token;
        if(receive(control[0],&token,1))_exit(1);
        for(unsigned i=0;i<8192;++i)if(bytes[i]!=0x3a)_exit(1);
        _exit(0);
    }
    close(control[0]);CHECK(write(data[1],"cow!",4)==4);
    CHECK(read(data[0],bytes,4)==4 && !memcmp(bytes,"cow!",4));
    CHECK(write(control[1],"x",1)==1);close(control[1]);if(child>0)joined(child);
    close(data[0]);close(data[1]);CHECK(munmap(bytes,8192)==0);
    puts("[pipe-parallel] lazy/COW destinations preserved");
}

/** @brief Query a monotonic timestamp in nanoseconds. */
static uint64_t now(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC,&value))return 0;
    return (uint64_t)value.tv_sec*1000000000u+(uint64_t)value.tv_nsec;
}

/** @brief Measure real PTY delivery latency while the shell runs pipe load. */
static int terminal(void)
{
    int master=posix_openpt(O_RDWR|O_NOCTTY);CHECK(master>=0);
    if(master<0)return 1;
    CHECK(grantpt(master)==0 && unlockpt(master)==0);
    char *name=ptsname(master);CHECK(name!=NULL);
    int slave=name ? open(name,O_RDWR|O_NOCTTY) : -1;CHECK(slave>=0);
    if(slave<0) { close(master);return 1; }
    struct termios attributes;CHECK(tcgetattr(slave,&attributes)==0);
    cfmakeraw(&attributes);CHECK(tcsetattr(slave,TCSANOW,&attributes)==0);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        close(master);struct timespec interval={.tv_nsec=100000000};
        for(unsigned i=0;i<60;++i) {
            uint64_t sent=now();if(write(slave,&sent,sizeof(sent))!=sizeof(sent))_exit(1);
            nanosleep(&interval,NULL);
        }
        close(slave);_exit(0);
    }
    close(slave);uint64_t sum=0,maximum=0;
    for(unsigned i=0;i<60;++i) {
        struct pollfd descriptor={.fd=master,.events=POLLIN};
        int ready;do { ready=poll(&descriptor,1,2000); } while(ready<0 && errno==EINTR);
        CHECK(ready==1 && (descriptor.revents&POLLIN));
        uint64_t sent=0;if(receive(master,&sent,sizeof(sent))) { CHECK(0);break; }
        uint64_t received=now();CHECK(received>=sent);
        uint64_t delay=received-sent;sum+=delay;if(delay>maximum)maximum=delay;
    }
    if(child>0)joined(child);close(master);
    printf("[pipe-pty] DONE samples=60 mean_us=%llu max_us=%llu failures=%u\n",
        (unsigned long long)(sum/60000u),(unsigned long long)(maximum/1000u),failures);
    return failures ? 1 : 0;
}

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);alarm(25);
    if(argc==2 && !strcmp(argv[1],"--pty"))return terminal();
    records(64);records(4096);destinations();
    printf("[pipe-parallel] DONE failures=%u\n",failures);
    return failures ? 1 : 0;
}

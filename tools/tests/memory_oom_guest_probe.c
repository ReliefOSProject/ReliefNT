/* SPDX-License-Identifier: Apache-2.0 */
/* Explicitly invoked destructive-pressure control for an owned ReliefNT guest. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE_BYTES 4096ULL
#define MAX_PROBE_BYTES (16ULL * 1024 * 1024 * 1024)
#define EXTRA_BYTES (64ULL * 1024 * 1024)

/** @brief Set and verify this process's standard OOM adjustment. */
static int set_adjustment(int value)
{
    FILE *file=fopen("/proc/self/oom_score_adj","w");
    if (!file) return -1;
    int written=fprintf(file,"%d\n",value), closed=fclose(file), observed=0;
    if (written<0 || closed) return -1;
    file=fopen("/proc/self/oom_score_adj","r");
    if (!file) return -1;
    int got=fscanf(file,"%d",&observed); closed=fclose(file);
    return got==1 && !closed && observed==value ? 0 : -1;
}

/** @brief Read one KiB field or monotonic count from a proc text file. */
static int read_counter(const char *path,const char *key,uint64_t *result)
{
    FILE *file=fopen(path,"r"); char line[256], name[64]; unsigned long long value;
    if (!file) return -1;
    int found=0;
    while (fgets(line,sizeof(line),file)) {
        if (sscanf(line,"%63s %llu",name,&value)==2 && !strcmp(name,key)) {
            *result=(uint64_t)value; found=1; break;
        }
    }
    int closed=fclose(file); return found && !closed ? 0 : -1;
}

/** @brief Obtain monotonic seconds for the bounded supervisor wait. */
static int64_t now_seconds(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC,&now) ? -1 : (int64_t)now.tv_sec;
}

/** @brief Create only an owned child and verify retirement and post-OOM memory. */
int main(int argc,char **argv)
{
    struct utsname system;
    if (argc!=4 || strcmp(argv[1],"--guest-oom") || uname(&system) || strcmp(system.sysname,"ReliefNT")) {
        fprintf(stderr,"usage on ReliefNT guest only: %s --guest-oom MAX_BYTES TIMEOUT_SECONDS\n",argv[0]); return 2;
    }
    char *end; errno=0; unsigned long long maximum=strtoull(argv[2],&end,10);
    if (errno || end==argv[2] || *end || argv[2][0]=='-' || !maximum || maximum>MAX_PROBE_BYTES) return 2;
    errno=0; unsigned long timeout=strtoul(argv[3],&end,10);
    if (errno || end==argv[3] || *end || argv[3][0]=='-' || !timeout || timeout>3600) return 2;
    setvbuf(stdout,NULL,_IONBF,0);
    uint64_t ram,swap,before,after;
    if (read_counter("/proc/meminfo","MemTotal:",&ram) ||
        read_counter("/proc/meminfo","SwapTotal:",&swap) ||
        read_counter("/proc/vmstat","oom_kill",&before) ||
        !ram || ram>UINT64_MAX-swap || ram+swap>(UINT64_MAX-EXTRA_BYTES)/1024) {
        fprintf(stderr,"OOM_PROBE_FAIL counters\n"); return 1;
    }
    uint64_t bytes=(ram+swap)*1024+EXTRA_BYTES;
    if (bytes>UINT64_MAX-(PAGE_BYTES-1)) return 1;
    bytes=(bytes+PAGE_BYTES-1)&~(PAGE_BYTES-1);
    if (bytes>maximum || bytes>SIZE_MAX) {
        fprintf(stderr,"OOM_PROBE_FAIL target=%" PRIu64 " cap=%llu\n",bytes,maximum); return 1;
    }
    if (set_adjustment(-1000)) { perror("protect supervisor"); return 1; }
    int gate[2]; if (pipe(gate)) { perror("pipe"); return 1; }
    pid_t child=fork();
    if (child<0) { perror("fork"); return 1; }
    if (!child) {
        close(gate[1]); char token;
        if (read(gate[0],&token,1)!=1 || set_adjustment(1000)) _exit(10);
        close(gate[0]);
        volatile unsigned char *p=mmap(NULL,(size_t)bytes,PROT_READ|PROT_WRITE,
                                       MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (p==MAP_FAILED) _exit(11);
        printf("OOM_PROBE_ARMED pid=%ld target=%" PRIu64 "\n",(long)getpid(),bytes);
        for (uint64_t offset=0;offset<bytes;offset+=PAGE_BYTES) p[offset]=(unsigned char)(offset/PAGE_BYTES+1);
        _exit(12); /* Touching more than total RAM+swap without OOM is failure. */
    }
    close(gate[0]);
    printf("OOM_PROBE_CHILD pid=%ld ram_kib=%" PRIu64 " swap_kib=%" PRIu64
           " target=%" PRIu64 " oom_before=%" PRIu64 "\n",(long)child,ram,swap,bytes,before);
    if (write(gate[1],"x",1)!=1) { close(gate[1]); kill(child,SIGKILL); return 1; }
    close(gate[1]);
    int64_t start=now_seconds(); int status=0; pid_t waited=0;
    if (start<0) { kill(child,SIGKILL); return 1; }
    while ((waited=waitpid(child,&status,WNOHANG))==0) {
        int64_t now=now_seconds();
        if (now<0 || now-start>=(int64_t)timeout) break;
        struct timespec pause={.tv_sec=0,.tv_nsec=250000000}; nanosleep(&pause,NULL);
    }
    if (waited!=child) {
        kill(child,SIGKILL); /* Only the owned child, and never counted as success. */
        for (unsigned i=0;i<40 && (waited=waitpid(child,&status,WNOHANG))==0;++i) {
            struct timespec pause={.tv_sec=0,.tv_nsec=250000000}; nanosleep(&pause,NULL);
        }
        fprintf(stderr,"OOM_PROBE_FAIL timeout_or_wait pid=%ld waited=%ld\n",(long)child,(long)waited); return 1;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL ||
        read_counter("/proc/vmstat","oom_kill",&after) || after<=before) {
        fprintf(stderr,"OOM_PROBE_FAIL status=%d counter\n",status); return 1;
    }
    size_t fresh=1024*1024;
    volatile unsigned char *p=mmap(NULL,fresh,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (p==MAP_FAILED) { perror("post-OOM mmap"); return 1; }
    for (size_t i=0;i<fresh;i+=PAGE_BYTES) p[i]=(unsigned char)(i/PAGE_BYTES+3);
    for (size_t i=0;i<fresh;i+=PAGE_BYTES) if (p[i]!=(unsigned char)(i/PAGE_BYTES+3)) return 1;
    if (munmap((void *)p,fresh)) return 1;
    printf("OOM_PROBE_PASS pid=%ld signal=%d oom_before=%" PRIu64 " oom_after=%" PRIu64
           " fresh_bytes=%zu\n",(long)child,SIGKILL,before,after,fresh);
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 4
#define CHUNK 65536
#define CHUNKS 32

/** @brief Produce a stable byte for one absolute file position.
 * @param offset Absolute position. @return Deterministic expected byte.
 */
static unsigned char pattern(uint64_t offset)
{
    return (unsigned char)((offset*17u)^(offset>>9)^(offset>>17)^0xa7u);
}

/** @brief Perform an exact positional transfer without hiding short/error results.
 * @param fd Owned file descriptor. @param buffer CHUNK valid bytes.
 * @param offset Absolute file position. @param writing Select pwrite over pread.
 * @return Zero or failure with a diagnostic on stderr.
 */
static int transfer(int fd,unsigned char *buffer,off_t offset,int writing)
{
    size_t done=0;
    while(done<CHUNK) {
        ssize_t n=writing ? pwrite(fd,buffer+done,CHUNK-done,offset+done) :
                           pread(fd,buffer+done,CHUNK-done,offset+done);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) {fprintf(stderr,"STORAGE_FAIL io offset=%lld write=%d n=%zd errno=%d\n",
                         (long long)offset,writing,n,errno);return 1;}
        done+=(size_t)n;
    }
    return 0;
}

/** @brief Verify a worker's disjoint extent through current positional reads.
 * @param fd Owned descriptor. @param worker Extent index below WORKERS.
 * @return Zero or data/transport failure; stack bytes never survive a syscall.
 */
static int verify(int fd,unsigned worker)
{
    unsigned char buffer[CHUNK];
    for(unsigned chunk=0;chunk<CHUNKS;++chunk) {
        off_t offset=((off_t)worker*CHUNKS+(CHUNKS-1-chunk))*CHUNK;
        if(transfer(fd,buffer,offset,0))return 1;
        for(unsigned i=0;i<CHUNK;++i)if(buffer[i]!=pattern(offset+i)) {
            fprintf(stderr,"STORAGE_FAIL data worker=%u offset=%lld got=%u expected=%u\n",
                    worker,(long long)offset+i,buffer[i],pattern(offset+i));return 1;
        }
    }
    return 0;
}

/** @brief Write, fsync and repeatedly verify one independent file extent.
 * @param path Existing disposable test file. @param worker Disjoint extent index.
 * @return Zero or open/write/flush/data failure, without altering other extents.
 */
static int worker_run(const char *path,unsigned worker)
{
    int fd=open(path,O_RDWR);
    if(fd<0){perror("STORAGE_FAIL open worker");return 1;}
    unsigned char buffer[CHUNK];
    for(unsigned chunk=0;chunk<CHUNKS;++chunk) {
        off_t offset=((off_t)worker*CHUNKS+chunk)*CHUNK;
        for(unsigned i=0;i<CHUNK;++i)buffer[i]=pattern(offset+i);
        if(transfer(fd,buffer,offset,1))return 1;
    }
    if(fsync(fd)){perror("STORAGE_FAIL fsync");return 1;}
    for(unsigned round=0;round<3;++round)if(verify(fd,worker))return 1;
    return close(fd)!=0;
}

/** @brief Exercise SMP continuations, flush/reopen and cancellation on an owned file.
 * @param argc Must be two. @param argv Test file path in disposable guest storage.
 * @return Zero only after all child statuses and bytes verify through POSIX I/O.
 */
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    int fd=open(argv[1],O_RDWR|O_CREAT|O_TRUNC,0600);
    if(fd<0 || ftruncate(fd,(off_t)WORKERS*CHUNKS*CHUNK)){perror("STORAGE_FAIL create");return 1;}
    if(close(fd))return 1;
    struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
    pid_t children[WORKERS];
    for(unsigned i=0;i<WORKERS;++i) {
        children[i]=fork();
        if(children[i]<0){perror("STORAGE_FAIL fork");return 1;}
        if(!children[i])_exit(worker_run(argv[1],i));
    }
    for(unsigned i=0;i<WORKERS;++i) {
        int status=0;
        if(waitpid(children[i],&status,0)!=children[i] || !WIFEXITED(status) || WEXITSTATUS(status))return 1;
    }
    fd=open(argv[1],O_RDONLY);if(fd<0)return 1;
    for(unsigned i=0;i<WORKERS;++i)if(verify(fd,i))return 1;
    close(fd);
    pid_t reader=fork();
    if(reader<0)return 1;
    if(!reader) {
        int input=open(argv[1],O_RDONLY);if(input<0)_exit(1);
        for(;;)if(verify(input,0))_exit(1);
    }
    struct timespec delay={.tv_nsec=20000000};nanosleep(&delay,NULL);
    int status=0;
    if(kill(reader,SIGKILL) || waitpid(reader,&status,0)!=reader ||
       !WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL)return 1;
    fd=open(argv[1],O_RDONLY);if(fd<0)return 1;
    for(unsigned i=0;i<WORKERS;++i)if(verify(fd,i))return 1;
    if(close(fd))return 1;
    clock_gettime(CLOCK_MONOTONIC,&end);
    double seconds=end.tv_sec-start.tv_sec+(end.tv_nsec-start.tv_nsec)/1e9;
    printf("STORAGE_GUEST_PASS path=%s workers=%u bytes=%u seconds=%.3f cancel=pass fsync=pass\n",
           argv[1],WORKERS,WORKERS*CHUNKS*CHUNK,seconds);
    return 0;
}

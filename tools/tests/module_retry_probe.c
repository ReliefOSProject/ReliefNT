/* SPDX-License-Identifier: Apache-2.0 */
/* Native ReliefNT only: final initializer errno versus pre-init storage retry. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifndef MODULE_RETRY_PATH
#define MODULE_RETRY_PATH "/init_fail.ko"
#endif

static void require(int okay,const char *operation)
{
    if(okay)return;
    printf("FAIL MODULE_RETRY operation=%s errno=%d (%s)\n",operation,errno,strerror(errno));
    if(getpid()==1)for(;;)pause();
    _exit(1);
}
static void require_absent(void)
{
    char listing[4096]={0};FILE *file=fopen("/proc/modules","r");require(file!=NULL,"open module list");
    size_t got=fread(listing,1,sizeof(listing)-1,file);require(!ferror(file),"read module list");
    fclose(file);listing[got]=0;require(strstr(listing,"init_fail ")==NULL,"failed module is not retained");
}
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT")) {
        fputs("Guest-only module errno probe: refusing host module operations\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0) {dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);
    printf("MODULE_RETRY_READY release=%s module=%s\n",identity.release,MODULE_RETRY_PATH);
    int fd=open(MODULE_RETRY_PATH,O_RDONLY|O_CLOEXEC);require(fd>=0,"open init-error module");
    struct stat info;require(!fstat(fd,&info) && info.st_size>0,"module size");
    size_t size=(size_t)info.st_size;unsigned char *image=malloc(size);require(image!=NULL,"module buffer");
    for(size_t position=0;position<size;) {
        ssize_t got=read(fd,image+position,size-position);require(got>0,"read module image");position+=(size_t)got;
    }
    require(lseek(fd,17,SEEK_SET)==17,"set shared file offset");
    int writer=open(MODULE_RETRY_PATH,O_RDWR|O_CLOEXEC);require(writer>=0,"open competing writer");
    errno=0;require(syscall(SYS_finit_module,writer,"failure=-11",0)==-1 && errno==ETXTBSY,
                    "read-write module descriptor rejected");
    int alias=dup(writer);require(alias>=0,"duplicate writer");close(writer);
    errno=0;require(syscall(SYS_finit_module,fd,"failure=-11",0)==-1 && errno==ETXTBSY,
                    "other writable OFD prevents file snapshot");
    size_t mapped_length=3*4096;
    void *mapping=mmap(NULL,mapped_length,PROT_READ,MAP_PRIVATE,alias,0);
    require(mapping!=MAP_FAILED,"map writable OFD read-only/private");close(alias);
    errno=0;require(syscall(SYS_finit_module,fd,"failure=-11",0)==-1 && errno==ETXTBSY,
                    "closed writer remains pinned by VMA");
    int ready[2],action[2];require(!pipe(ready) && !pipe(action),"create mapping lifetime channels");
    pid_t child=fork();require(child>=0,"fork mapped writer");
    if(!child) {
        close(ready[0]);close(action[1]);
        require(write(ready[1],"R",1)==1,"child mapping ready");char command=0;
        require(read(action[0],&command,1)==1 && command=='U',"wait to unmap child");
        require(!munmap(mapping,mapped_length),"child final unmap");_exit(0);
    }
    close(ready[1]);close(action[0]);char response=0;
    require(read(ready[0],&response,1)==1 && response=='R',"observe inherited VMA");
    require(!munmap((char *)mapping+4096,4096),"split parent file VMA");
    require(!munmap(mapping,4096) && !munmap((char *)mapping+8192,4096),"retire parent VMA segments");
    errno=0;require(syscall(SYS_finit_module,fd,"failure=-11",0)==-1 && errno==ETXTBSY,
                    "forked child VMA still prevents snapshot");
    require(write(action[1],"U",1)==1,"release child mapping");int status=0;
    require(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"reap mapping owner");
    close(ready[0]);close(action[1]);
    puts("MODULE_RETRY_WRITER_LEASES_OK fd-dup-close-private-VMA-split-fork-final-unmap");
    for(unsigned cycle=1;cycle<=4;++cycle) {
        errno=0;
        require(syscall(SYS_finit_module,fd,"failure=-11",0)==-1 && errno==EAGAIN,
                "finit initializer EAGAIN returned once");
        require(lseek(fd,0,SEEK_CUR)==17,"positional module read preserves OFD offset");require_absent();
        errno=0;
        require(syscall(SYS_init_module,image,size,"failure=-11")==-1 && errno==EAGAIN,
                "init initializer EAGAIN returned once");
        require_absent();printf("MODULE_RETRY_CYCLE=%u final-EAGAIN-and-complete-rollback\n",cycle);
    }
    free(image);close(fd);
    puts("PASS MODULE_RETRY four init/finit EAGAIN cycles, no initializer replay, no retained image, positional OFD");
    if(getpid()==1)for(;;)pause();
    return 0;
}

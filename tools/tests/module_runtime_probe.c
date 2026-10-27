/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/stat.h>

static void require(int okay,const char *operation)
{
    if(!okay){printf("FAIL MODULE_RUNTIME operation=%s errno=%d (%s)\n",operation,errno,strerror(errno));
        if(getpid()==1)for(;;)pause();
        _exit(1);
    }
}
static void expect_socket_batches(void)
{
    int endpoints[2];require(!socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,endpoints),"create native message pair");
    char messages[3][4]={"one","two","end"},received[3][4]={{0}};
    struct iovec outgoing[3],incoming[3];struct mmsghdr sends[3]={0},receives[3]={0};
    for(unsigned i=0;i<3;++i) {
        outgoing[i]=(struct iovec){messages[i],3};incoming[i]=(struct iovec){received[i],4};
        sends[i].msg_hdr=(struct msghdr){.msg_iov=&outgoing[i],.msg_iovlen=1};
        receives[i].msg_hdr=(struct msghdr){.msg_iov=&incoming[i],.msg_iovlen=1};
    }
    require(sendmmsg(endpoints[0],sends,3,MSG_NOSIGNAL)==3,"send native scoped message batch");
    require(recvmmsg(endpoints[1],receives,3,MSG_DONTWAIT,NULL)==3,"receive native batch boundaries");
    for(unsigned i=0;i<3;++i)require(sends[i].msg_len==3 && receives[i].msg_len==3 &&
        !memcmp(messages[i],received[i],3),"verify native batch lengths and contents");
    require(!close(endpoints[0]) && !close(endpoints[1]),"release native message pair");
    puts("MODULE_RUNTIME_SOCKET_BATCH native sendmmsg/recvmmsg, scoped headers/vectors, exact datagrams");
}
static void expect_live(void)
{
    char buffer[1024]={0};
    int fd=open("/proc/modules",O_RDONLY);require(fd>=0,"open proc modules");
    ssize_t n=read(fd,buffer,sizeof(buffer)-1);close(fd);
    require(n>0 && strstr(buffer,"standalone ") && strstr(buffer," Live "),"proc module live record");
    fd=open("/sys/module/standalone/initstate",O_RDONLY);require(fd>=0,"open sys module initstate");
    n=read(fd,buffer,sizeof(buffer)-1);close(fd);
    require(n==5 && !memcmp(buffer,"live\n",5),"sysfs live state");
}
static void choose_cpu(int cpu)
{
    if(cpu<0)return;
    cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);
    require(!sched_setaffinity(0,sizeof(set),&set),"set affinity");
}
static void expect_volatile_module(const void *bytes,size_t length)
{
    struct statfs fs;
    struct stat metadata;
    require(!statfs("/dev/shm",&fs) && fs.f_type==0x01021994,"volatile shm filesystem");
    require(!stat("/dev/shm",&metadata) && (metadata.st_mode&07777)==01777 &&
            !metadata.st_uid && !metadata.st_gid,"volatile shm root ownership");
    int writer=open("/dev/shm/native-module.ko",O_WRONLY|O_CREAT|O_EXCL,0600);
    require(writer>=0,"create volatile module image");
    size_t done=0;
    while(done<length){ssize_t n=write(writer,(const char *)bytes+done,length-done);
        require(n>0,"write volatile module image");done+=(size_t)n;}
    require(!close(writer),"close volatile writer before module load");
    int reader=open("/dev/shm/native-module.ko",O_RDONLY);
    require(reader>=0 && lseek(reader,17,SEEK_SET)==17,"open volatile module at nonzero offset");
    require(!unlink("/dev/shm/native-module.ko"),"unlink held volatile module image");
    require(!syscall(SYS_finit_module,reader,"",0),"load unlinked volatile module");
    require(lseek(reader,0,SEEK_CUR)==17,"finit preserves volatile OFD offset");
    expect_live();
    require(!syscall(SYS_delete_module,"standalone",O_NONBLOCK),"unload volatile module");
    require(!close(reader),"release unlinked volatile image");
    puts("MODULE_RUNTIME_VOLATILE tmpfs mode=1777 unlinked-finit offset=17");
}
static void expect_directory_mutations(int parent)
{
    require(!mkdirat(parent,"mutation-child",0750),"mkdirat uses actual renamed directory");
    int directory=openat(parent,"mutation-child",O_RDONLY|O_DIRECTORY);
    require(directory>=0,"open created child through held parent");
    int file=openat(directory,"nested",O_RDWR|O_CREAT|O_EXCL,0600);
    require(file>=0 && write(file,"held-data",9)==9,"populate child mutation file");
    require(!symlinkat("nested",directory,"relative"),"symlinkat uses actual directory object");
    require(!mkfifoat(directory,"fifo",0600),"mknodat creates FIFO in actual directory");
    struct stat attributes;
    require(!fstatat(directory,"fifo",&attributes,0) && S_ISFIFO(attributes.st_mode),"created FIFO inode type");
    require(!renameat(parent,"mutation-child",parent,"mutation-moved") &&
        !renameat(parent,"mutation-moved",parent,"mutation-child"),"renameat uses actual retained parent");
    require(!linkat(directory,"nested",directory,"hard-alias",0),"linkat uses actual renamed directory");
    require(!linkat(file,"",directory,"fd-alias",AT_EMPTY_PATH),"linkat empty source uses exact inode");
    require(!linkat(directory,"relative",directory,"symlink-alias",0) &&
        !fstatat(directory,"symlink-alias",&attributes,AT_SYMLINK_NOFOLLOW) && S_ISLNK(attributes.st_mode) &&
        attributes.st_nlink==2,"hardlink preserves final symlink inode");
    require(!linkat(directory,"relative",directory,"follow-alias",AT_SYMLINK_FOLLOW) &&
        !fstat(file,&attributes) && attributes.st_nlink==4,"hardlink follows source only when requested");
    require(!unlinkat(directory,"hard-alias",0) && !unlinkat(directory,"fd-alias",0) &&
        !unlinkat(directory,"symlink-alias",0) && !unlinkat(directory,"follow-alias",0),"release every hardlink alias");
    puts("MODULE_RUNTIME_CREATE renamed-dirfd mkdir/symlink/FIFO/renameat/linkat flags");
    errno=0;require(unlinkat(parent,"mutation-child",AT_REMOVEDIR)==-1 && errno==ENOTEMPTY,
        "rmdir rejects actual nonempty directory");
    require(!unlinkat(directory,"relative",0) && !unlinkat(directory,"fifo",0) &&
        !unlinkat(directory,"nested",0),"unlinkat uses actual child directory");
    char bytes[10]={0};
    require(!fstat(file,&attributes) && !attributes.st_nlink && pread(file,bytes,9,0)==9 && !memcmp(bytes,"held-data",9),
        "unlinked child inode survives through held fd");
    require(!unlinkat(parent,"mutation-child",AT_REMOVEDIR),"remove actual empty child directory");
    require(!fchdir(directory),"enter still-held removed directory");
    char cwd[256];errno=0;
    require(syscall(SYS_getcwd,cwd,sizeof(cwd))==-1 && errno==ENOENT,"removed cwd has no namespace name");
    errno=0;require(mkdir("lost",0700)==-1 && errno==ENOENT,"removed cwd rejects new members");
    require(!chdir("/") && !close(file) && !close(directory),"release removed directory and child inode");
    puts("MODULE_RUNTIME_MUTATIONS renamed-dirfd mkdir/symlink/FIFO/unlink/rmdir held-inode/deleted-cwd");
}
static void expect_unix_inode_binding(int parent)
{
    require(!fchdir(parent),"socket bind uses renamed held cwd");
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,"socket-original");
    int listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0);
    mode_t saved_umask=umask(0077);
    require(listener>=0 && !bind(listener,(struct sockaddr *)&address,sizeof(address)),"bind actual socket inode");
    umask(saved_umask);
    struct stat attributes;
    require(!fstatat(parent,"socket-original",&attributes,0) && S_ISSOCK(attributes.st_mode) &&
        (attributes.st_mode&07777)==0700,"socket mode applies umask");
    require(!listen(listener,4),"listen on held socket inode");
    require(!rename("/dev/shm/module-moved/socket-original","/dev/shm/module-moved/socket-renamed"),"rename bound socket inode");
    require(!linkat(parent,"socket-renamed",parent,"socket-alias",0),"hardlink bound socket through retained directory");
    strcpy(address.sun_path,"socket-alias");
    int client=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0);
    require(client>=0 && !connect(client,(struct sockaddr *)&address,sizeof(address)),"connect socket through hardlink inode alias");
    int accepted=accept4(listener,NULL,NULL,SOCK_NONBLOCK);require(accepted>=0,"accept alias connection");
    require(!unlinkat(parent,"socket-alias",0) && !unlinkat(parent,"socket-renamed",0),"unlink socket names through held cwd");
    strcpy(address.sun_path,"socket-renamed");
    int replacement=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0);
    require(replacement>=0 && !bind(replacement,(struct sockaddr *)&address,sizeof(address)) && !listen(replacement,4),
        "rebind removed name to a new socket inode");
    int fresh=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0);
    require(fresh>=0 && !connect(fresh,(struct sockaddr *)&address,sizeof(address)),"connect resolves new inode after rebinding");
    int new_accepted=accept4(replacement,NULL,NULL,SOCK_NONBLOCK);
    require(new_accepted>=0,"new inode selects new listener");
    char data[4]={0};
    require(write(client,"old",3)==3 && read(accepted,data,3)==3 && !memcmp(data,"old",3),
        "unlinked old socket connection survives rebinding");
    require(write(fresh,"new",3)==3 && read(new_accepted,data,3)==3 && !memcmp(data,"new",3),"new socket connection remains independent");
    require(!unlinkat(parent,"socket-renamed",0),"remove replacement socket inode");
    require(!close(client) && !close(accepted) && !close(fresh) && !close(new_accepted) &&
        !close(listener) && !close(replacement) && !chdir("/"),"release every socket and retained inode");
    puts("MODULE_RUNTIME_UNIX inode alias/rename/unlink/rebind and independent live streams");
}
static void expect_directory_module(const void *bytes,size_t length)
{
    require(!mkdir("/dev/shm/module-source",0750),"create module source directory");
    int directory=open("/dev/shm/module-source",O_RDONLY|O_DIRECTORY);
    require(directory>=0,"hold module directory");
    int writer=openat(directory,"held.ko",O_WRONLY|O_CREAT|O_EXCL,0600);
    require(writer>=0,"create module through held directory");
    size_t done=0;
    while(done<length){ssize_t n=write(writer,(const char *)bytes+done,length-done);
        require(n>0,"write directory module");done+=(size_t)n;}
    require(!close(writer),"close directory module writer");
    require(!symlinkat("held.ko",directory,"relative.ko"),"create held-directory symlink");
    require(!rename("/dev/shm/module-source","/dev/shm/module-moved"),"rename held module directory");
    require(!fchdir(directory),"enter renamed held directory");
    char renamed_cwd[256];
    require(syscall(SYS_getcwd,renamed_cwd,sizeof(renamed_cwd))>0 &&
        !strcmp(renamed_cwd,"/dev/shm/module-moved"),"rename updates held cwd graph name");
    require(!chdir("/"),"return from renamed cwd");
    int reader=openat(directory,"held.ko",O_RDONLY);
    require(reader>=0,"openat follows held directory after rename");
    struct stat attributes,original;
    require(!fstat(reader,&original) && !fstatat(directory,"held.ko",&attributes,0) &&
        attributes.st_size==(off_t)length && attributes.st_ino==original.st_ino,"statat follows renamed held directory");
    require(!faccessat(directory,"held.ko",R_OK,0),"accessat follows renamed held directory");
    require(!fchmodat(directory,"held.ko",0640,0),"chmodat follows renamed held directory");
    require(!fchownat(directory,"held.ko",123,456,0),"chownat follows renamed held directory");
    require(!fstat(reader,&attributes) && (attributes.st_mode&07777)==0640 &&
        attributes.st_uid==123 && attributes.st_gid==456,"bound ownership visible on held fd");
    struct timespec times[2]={{1700000101,0},{1700000202,0}};
    require(!utimensat(directory,"held.ko",times,0) && !fstat(reader,&attributes) &&
        attributes.st_atim.tv_sec==1700000101 && attributes.st_mtim.tv_sec==1700000202,
        "utimensat follows renamed held directory");
    int sized=openat(directory,"size.bin",O_CREAT|O_EXCL|O_RDWR,0600);
    require(sized>=0 && write(sized,"abcd",4)==4,"populate held cwd size fixture");
    require(!fchdir(directory) && !truncate("size.bin",2),"truncate follows actual renamed cwd");
    require(!ftruncate(sized,6) && !fstat(sized,&attributes) && attributes.st_size==6,
        "ftruncate updates held inode size");
    char resized[6]={1,1,1,1,1,1};
    require(pread(sized,resized,sizeof(resized),0)==6 && !memcmp(resized,"ab\0\0\0\0",6),
        "held truncation zero-fills growth");
    require(!fallocate(sized,1,4096,8192) && !fstat(sized,&attributes) &&
        attributes.st_size==6 && attributes.st_blocks==24,"held tmpfs KEEP_SIZE reserves backing");
    require(!unlinkat(directory,"size.bin",0),"unlink held preallocated file");
    require(!fallocate(sized,0,0,12288) && !fstat(sized,&attributes) &&
        attributes.st_size==12288 && !attributes.st_nlink,"preallocate original unlinked inode");
    require(!fsync(sized) && !fdatasync(sized) && !syncfs(sized),"sync held unlinked tmpfs inode");
    int syncpath=openat(directory,".",O_PATH|O_DIRECTORY);
    require(syncpath>=0 && !syncfs(syncpath),"syncfs accepts O_PATH to actual filesystem");
    errno=0;require(syscall(SYS_fsync,syncpath)==-1 && errno==EBADF,"fsync rejects O_PATH");
    require(!close(syncpath),"close syncfs path");
    require(!chdir("/") && !close(sized),"release size fixture");
    char target[32]={0};
    require(readlinkat(directory,"relative.ko",target,sizeof(target))==7 && !memcmp(target,"held.ko",7),"readlinkat follows renamed held directory");
    expect_directory_mutations(directory);
    expect_unix_inode_binding(directory);
    require(!syscall(SYS_finit_module,reader,"",0),"load from renamed directory fd");
    expect_live();
    require(!syscall(SYS_delete_module,"standalone",O_NONBLOCK),"unload directory module");
    require(!close(reader) && !close(directory),"release directory module descriptors");
    require(!unlink("/dev/shm/module-moved/held.ko") &&
        !unlink("/dev/shm/module-moved/relative.ko") && !rmdir("/dev/shm/module-moved"),"remove directory module artifacts");
    puts("MODULE_RUNTIME_DIRECTORY renamed-dirfd openat/finit");
}
static void expect_open_contract(void)
{
    int memory=(int)syscall(SYS_memfd_create,"module-open-contract",2u);
    require(memory>=0 && write(memory,"abc",3)==3,"populate anonymous reopen source");
    char name[80],bytes[4]={0};snprintf(name,sizeof(name),"/proc/self/fd/%d",memory);
    int reader=open(name,O_RDONLY);
    require(reader>=0 && read(reader,bytes,3)==3 && !memcmp(bytes,"abc",3),"reopen exact anonymous inode");
    require(!close(reader),"close anonymous reader");
    reader=open(name,O_RDONLY|O_TRUNC);
    require(reader>=0,"readonly truncation of writable anonymous inode");
    struct stat metadata;
    require(!fstat(memory,&metadata) && !metadata.st_size,"anonymous truncation visible through original fd");
    require(!close(reader) && !close(memory),"close anonymous descriptions");
    int file=open("/dev/shm/open-contract",O_CREAT|O_EXCL|O_RDWR,0600);
    require(file>=0 && write(file,"abc",3)==3 && !close(file),"populate open flag fixture");
    file=open("/dev/shm/open-contract",O_PATH|O_TRUNC|O_WRONLY|O_CREAT|O_CLOEXEC);
    require(file>=0 && !fstat(file,&metadata) && metadata.st_size==3,"O_PATH ignores write/create/truncate");
    require(fcntl(file,F_GETFD)==FD_CLOEXEC,"O_PATH close-on-exec flag");
    /* musl deliberately emulates fchmod(O_PATH) via /proc after raw EBADF. */
    errno=0;require(syscall(SYS_fchmod,file,0644)==-1 && errno==EBADF,"O_PATH raw fchmod rejected");
    errno=0;require(read(file,bytes,1)==-1 && errno==EBADF,"O_PATH read rejected");
    errno=0;require(lseek(file,0,SEEK_SET)==-1 && errno==EBADF,"O_PATH seek rejected");
    errno=0;require(syscall(SYS_ftruncate,file,0)==-1 && errno==EBADF,"O_PATH truncate rejected");
    require(!close(file),"close O_PATH");
    require(!symlink("open-contract","/dev/shm/open-contract-link"),"create final symlink fixture");
    errno=0;require(open("/dev/shm/open-contract-link",O_RDONLY|O_NOFOLLOW)==-1 && errno==ELOOP,"nofollow final symlink rejected");
    errno=0;require(open("/dev/shm/open-contract-link",O_WRONLY|O_CREAT|O_EXCL,0600)==-1 && errno==EEXIST,"exclusive create preserves symlink");
    file=open("/dev/shm/open-contract-link",O_PATH|O_NOFOLLOW);
    require(file>=0 && !fstat(file,&metadata) && S_ISLNK(metadata.st_mode),"O_PATH retains final symlink");
    char target[32]={0};
    require(readlinkat(file,"",target,sizeof(target))==13 && !memcmp(target,"open-contract",13),"readlinkat empty path retains symlink");
    require(!close(file),"close symlink O_PATH");
    require(!unlink("/dev/shm/open-contract-link") && !unlink("/dev/shm/open-contract"),"remove open flag fixture");
    puts("MODULE_RUNTIME_OPEN anonymous-reopen/truncate O_PATH nofollow exclusive");
}
static void expect_metadata_contract(void)
{
    int memory=(int)syscall(SYS_memfd_create,"module-metadata-contract",2u);
    require(memory>=0 && write(memory,"meta",4)==4,"populate metadata anonymous inode");
    struct linux_statx extended;
    require(!syscall(SYS_statx,memory,"",AT_EMPTY_PATH,0,&extended) &&
        (extended.stx_mask&LINUX_STATX_BASIC_STATS)==LINUX_STATX_BASIC_STATS &&
        extended.stx_size==4,"statx accepts zero mask and reports available fields");
    require(!syscall(SYS_statx,memory,NULL,AT_EMPTY_PATH,LINUX_STATX_SIZE,&extended) && extended.stx_size==4,
        "statx null empty-path descriptor");
    errno=0;require(syscall(SYS_statx,memory,"",AT_EMPTY_PATH,0x80000000u,&extended)==-1 && errno==EINVAL,
        "statx reserved mask rejected");
    errno=0;require(syscall(SYS_statx,memory,"",AT_EMPTY_PATH|0x6000,0,&extended)==-1 && errno==EINVAL,
        "statx incompatible sync flags rejected");
    struct stat metadata;
    require(!syscall(SYS_newfstatat,memory,NULL,&metadata,AT_EMPTY_PATH) && metadata.st_size==4 && !metadata.st_nlink,
        "fstatat null empty-path anonymous inode");
    require(!fchmod(memory,0640) && !fchown(memory,321,654) && !fstat(memory,&metadata) &&
        (metadata.st_mode&07777)==0640 && metadata.st_uid==321 && metadata.st_gid==654,
        "anonymous fd ownership updates same inode");
    struct statfs fs;
    require(!fstatfs(memory,&fs) && fs.f_type==0x01021994,"anonymous fd filesystem statistics");
    require(!fallocate(memory,1,4096,4096) && !fstat(memory,&metadata) &&
        metadata.st_size==4 && metadata.st_blocks==16,"memfd preallocates actual backing without growing");
    require(!fsync(memory) && !fdatasync(memory) && !syncfs(memory),"anonymous shmem sync completes");
    require(!fcntl(memory,F_ADD_SEALS,F_SEAL_SHRINK|F_SEAL_GROW),"seal anonymous inode size");
    errno=0;require(ftruncate(memory,2)==-1 && errno==EPERM,"held truncate honors shrink seal");
    errno=0;require(ftruncate(memory,6)==-1 && errno==EPERM,"held truncate honors growth seal");
    errno=0;require(fallocate(memory,1,4096,4096)==-1 && errno==EPERM,"KEEP_SIZE honors grow seal beyond EOF");
    require(!fallocate(memory,0,0,4),"sealed memfd may reserve unchanged contents");
    require(!close(memory),"release metadata anonymous inode");
    int pipefd[2];require(!pipe(pipefd),"create non-directory descriptor fixture");
    errno=0;require(fstatat(pipefd[0],"child",&metadata,0)==-1 && errno==ENOTDIR,
        "pathless pipe is not a directory");
    require(!fstatat(pipefd[0],"",&metadata,AT_EMPTY_PATH) && S_ISFIFO(metadata.st_mode),
        "empty-path metadata on anonymous pipe");
    require(!close(pipefd[0]) && !close(pipefd[1]),"release metadata pipe");
    puts("MODULE_RUNTIME_METADATA statx masks/null, anonymous permissions/statfs, pipe at-path errors");
}
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT")) {
        fputs("Guest-only module probe: refusing host module operations\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0){dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);
    printf("MODULE_RUNTIME_READY release=%s\n",identity.release);
    cpu_set_t available;CPU_ZERO(&available);
    require(!sched_getaffinity(0,sizeof(available),&available),"read affinity");
    int first=-1,second=-1;
    for(int i=0;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&available)){if(first<0)first=i;else{second=i;break;}}
    if(second<0)second=first;
    choose_cpu(first);
    expect_socket_batches();
    expect_metadata_contract();
    int fd=open("/standalone.ko",O_RDONLY);require(fd>=0,"open module image");
    struct stat info;require(!fstat(fd,&info) && info.st_size>0,"module stat");
    void *bytes=malloc((size_t)info.st_size);require(bytes!=NULL,"allocate module snapshot");
    size_t have=0;while(have<(size_t)info.st_size){ssize_t n=read(fd,(char *)bytes+have,(size_t)info.st_size-have);require(n>0,"read module");have+=(size_t)n;}
    expect_volatile_module(bytes,have);
    expect_directory_module(bytes,have);
    expect_open_contract();
    int retired_attribute=-1;
    for(unsigned iteration=0;iteration<8;++iteration){
        int pipefd[2];require(!pipe(pipefd),"create barrier");
        pid_t child=fork();require(child>=0,"fork before load");
        if(!child){
            close(pipefd[1]);char go;require(read(pipefd[0],&go,1)==1,"load barrier");close(pipefd[0]);
            choose_cpu(second);
            require(!syscall(SYS_delete_module,"standalone",O_NONBLOCK),"unload from preexisting CR3 on another CPU");_exit(0);
        }
        close(pipefd[0]);
        long result=iteration&1 ? syscall(SYS_init_module,bytes,have,"") : syscall(SYS_finit_module,fd,"",0);
        require(!result,"load native module");expect_live();
        if(retired_attribute>=0) {
            char stale[16];errno=0;
            require(pread(retired_attribute,stale,sizeof(stale),0)==-1 && errno==ENOENT,
                "old sysfs inode never retargets reloaded module name");
            require(!close(retired_attribute),"close retired module attribute");
        }
        retired_attribute=open("/sys/module/standalone/initstate",O_RDONLY);
        require(retired_attribute>=0,"retain current sysfs module instance");
        errno=0;require(syscall(SYS_init_module,bytes,have,"")==-1 && errno==EEXIST,"duplicate rejected");
        require(write(pipefd[1],"x",1)==1,"wake child");close(pipefd[1]);
        int status;require(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"child unload status");
        errno=0;require(syscall(SYS_delete_module,"standalone",O_NONBLOCK)==-1 && errno==ENOENT,"removed module absent");
        printf("MODULE_RUNTIME_CYCLE=%u\n",iteration+1);
    }
    require(!close(retired_attribute),"release final retired module instance");
    close(fd);free(bytes);
    puts("PASS MODULE_RUNTIME native init/finit/delete, high shared mapping, cross-CPU existing CR3, BSS/RO, duplicate, proc/sysfs, virtual reuse");
    if(getpid()==1)for(;;)pause();
    return 0;
}

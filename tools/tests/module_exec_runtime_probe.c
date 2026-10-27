/* SPDX-License-Identifier: Apache-2.0 */
/* Selected-PID1 guest probe. Companion dynamic image uses PT_INTERP loader.so. */
#define main module_previous_probe
#include "module_runtime_probe.c"
#undef main

static void executable_identity_probe(void)
{
    struct stat original,missing,held;
    require(!stat("/qa-init",&original),"stat private test init");
    struct stat running;
    require(!stat("/proc/self/exe",&running) && running.st_dev==original.st_dev &&
        running.st_ino==original.st_ino,"rename only the actually selected private test init");
    errno=0;require(lstat("/qa-init-renamed",&missing)==-1 && errno==ENOENT,"reserve private executable rename destination");
    require(!rename("/qa-init","/qa-init-renamed"),"rename running private executable");
    errno=0;int blocked=open("/qa-init-renamed",O_WRONLY);
    require(blocked==-1 && errno==ETXTBSY,"running executable inode rejects writers after rename");
    int shadow=open("/qa-init",O_CREAT|O_EXCL|O_WRONLY,0600);
    require(shadow>=0 && write(shadow,"shadow",6)==6 && !close(shadow),"replace old executable name with another inode");
    char target[4096]={0};ssize_t size=readlink("/proc/self/exe",target,sizeof(target)-1);
    printf("MODULE_RUNTIME_EXECUTABLE_LINK count=%ld target=%s\n",(long)size,size<0 ? "<error>" : target);
    require(size==(ssize_t)strlen("/qa-init-renamed") && !memcmp(target,"/qa-init-renamed",size),
        "proc exe displays actual renamed running inode");
    int executable=open("/proc/self/exe",O_RDONLY);
    require(executable>=0 && !fstat(executable,&held) && held.st_ino==original.st_ino &&
        held.st_dev==original.st_dev && held.st_size==original.st_size,"proc exe opens actual retained inode rather than replacement name");
    require(!close(executable) && !unlink("/qa-init") && !rename("/qa-init-renamed","/qa-init"),"restore only private init names");
    puts("MODULE_RUNTIME_EXECUTABLE actual renamed /proc/self/exe path and magic inode");
}
static void wait_executed(pid_t child,const char *operation)
{
    int status=0;
    require(child>0 && waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==42,operation);
}
static void copy_guest_file(int directory,const char *name,const char *source_name)
{
    int source=open(source_name,O_RDONLY),writer=openat(directory,name,O_WRONLY|O_CREAT|O_EXCL,0700);
    require(source>=0 && writer>=0,"open private image copy handles");
    char bytes[32768];ssize_t got;
    while((got=read(source,bytes,sizeof(bytes)))>0) {
        ssize_t done=0;
        while(done<got){ssize_t n=write(writer,bytes+done,(size_t)(got-done));require(n>0,"copy exact image bytes");done+=n;}
    }
    require(!got && !close(source) && !close(writer),"finish image copy");
}
static void executable_directory_probe(void)
{
    require(!mkdir("/dev/shm/exe-source",0700),"create private executable directory");
    int directory=open("/dev/shm/exe-source",O_RDONLY|O_DIRECTORY);
    require(directory>=0,"hold private executable directory");
    copy_guest_file(directory,"program","/qa-init");
    copy_guest_file(directory,"dynamic","/qa-exec-dynamic");
    copy_guest_file(directory,"loader.so","/lib/ld-musl-x86_64.so.1");
    require(!fchdir(directory),"enter executable directory before rename");
    require(!rename("/dev/shm/exe-source","/dev/shm/exe-moved"),"rename held executable directory");
    char *arguments[]={"program","--exec-path-child","/dev/shm/exe-moved/program",NULL};
    char *environment[]={NULL};
    pid_t child=fork();require(child>=0,"fork held-dir executable child");
    if(!child){syscall(SYS_execveat,directory,"program",arguments,environment,0);perror("held-dir execveat");_exit(1);}
    wait_executed(child,"execveat uses renamed held directory inode");
    int path=openat(directory,"program",O_PATH);require(path>=0,"hold executable O_PATH");
    child=fork();require(child>=0,"fork empty-path executable child");
    if(!child){syscall(SYS_execveat,path,"",arguments,environment,AT_EMPTY_PATH);perror("empty-path execveat");_exit(1);}
    wait_executed(child,"execveat AT_EMPTY_PATH uses held O_PATH inode");require(!close(path),"close O_PATH executable");
    struct stat directory_status={0};char directory_link[4096]={0},descriptor_path[64];
    snprintf(descriptor_path,sizeof(descriptor_path),"/proc/self/fd/%d",directory);
    int directory_result=fstat(directory,&directory_status);
    ssize_t directory_size=readlink(descriptor_path,directory_link,sizeof(directory_link)-1);
    printf("MODULE_EXEC_DIAG dirfd=%d pathfd=%d stat=%d mode=%o linklen=%ld target=%s\n",directory,path,directory_result,(unsigned)directory_status.st_mode,(long)directory_size,directory_link);
    int writer=openat(directory,"program",O_WRONLY);require(writer>=0,"hold writable executable OFD");
    child=fork();require(child>=0,"fork writer-exclusion exec child");
    if(!child){errno=0;long rc=syscall(SYS_execveat,directory,"program",arguments,environment,0);_exit(rc==-1 && errno==ETXTBSY ? 42 : 1);}
    wait_executed(child,"execveat rejects an inode with a live writer");require(!close(writer),"release executable writer");
    require(!symlinkat("program",directory,"link"),"create executable link");
    child=fork();require(child>=0,"fork nofollow executable child");
    if(!child){errno=0;long rc=syscall(SYS_execveat,directory,"link",arguments,environment,AT_SYMLINK_NOFOLLOW);_exit(rc==-1 && errno==ELOOP ? 42 : 1);}
    wait_executed(child,"execveat nofollow rejects final symlink");
    writer=openat(directory,"script",O_WRONLY|O_CREAT|O_EXCL,0700);
    const char script[]="#!/dev/shm/exe-moved/program\n";
    require(writer>=0 && write(writer,script,sizeof(script)-1)==(ssize_t)sizeof(script)-1 && !close(writer),"create native interpreter script");
    child=fork();require(child>=0,"fork held-dir script child");
    if(!child){syscall(SYS_execveat,directory,"script",arguments,environment,0);perror("held-dir script execveat");_exit(1);}
    wait_executed(child,"shebang follows actual interpreter and retains original script argument");
    int script_fd=openat(directory,"script",O_RDONLY|O_CLOEXEC);require(script_fd>=0,"hold close-on-exec script");
    child=fork();require(child>=0,"fork close-on-exec script child");
    if(!child){errno=0;long rc=syscall(SYS_execveat,script_fd,"",arguments,environment,AT_EMPTY_PATH);_exit(rc==-1 && errno==ENOENT ? 42 : 1);}
    wait_executed(child,"execveat close-on-exec script is inaccessible");require(!close(script_fd),"close script handle");
    int ready[2],ack[2];require(!pipe(ready) && !pipe(ack),"create interpreter lifetime channels");
    char notify_number[24],ack_number[24];
    snprintf(notify_number,sizeof(notify_number),"%d",ready[1]);
    snprintf(ack_number,sizeof(ack_number),"%d",ack[0]);
    char *dynamic_arguments[]={"dynamic","--exec-path-child","/dev/shm/exe-moved/dynamic",notify_number,ack_number,NULL};
    child=fork();require(child>=0,"fork relative-interpreter child");
    if(!child){close(ready[0]);close(ack[1]);syscall(SYS_execveat,directory,"dynamic",dynamic_arguments,environment,0);perror("relative interpreter execveat");_exit(1);}
    require(!close(ready[1]) && !close(ack[0]),"retire parent channel aliases");
    char byte=0;require(read(ready[0],&byte,1)==1 && byte=='R',"PT_INTERP enters through actual renamed cwd");
    errno=0;writer=openat(directory,"loader.so",O_WRONLY);
    require(writer==-1 && errno==ETXTBSY,"live interpreter VMAs reject writers");
    require(!renameat(directory,"loader.so",directory,"retained-loader.so"),"rename live interpreter without changing backing");
    require(write(ack[1],"A",1)==1 && !close(ack[1]) && !close(ready[0]),"resume interpreter after rename");
    wait_executed(child,"PT_INTERP retained provider survives rename");
    writer=openat(directory,"retained-loader.so",O_WRONLY);
    require(writer>=0 && !close(writer),"last interpreter VMA releases writer exclusion");
    require(!renameat(directory,"retained-loader.so",directory,"loader.so"),"restore private interpreter name");
    require(!unlinkat(directory,"dynamic",0) && !unlinkat(directory,"loader.so",0),"remove private dynamic images");
    require(!unlinkat(directory,"link",0) && !unlinkat(directory,"script",0) && !unlinkat(directory,"program",0) &&
        !close(directory) && !chdir("/") && !rmdir("/dev/shm/exe-moved"),"remove only private executable fixture");
    puts("MODULE_RUNTIME_EXECVEAT renamed-dirfd, O_PATH/AT_EMPTY_PATH, nofollow, shebang and CLOEXEC");
}
int main(int argc,char **argv)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT")) {
        fputs("Guest-only executable path probe: refusing host mutations\n",stderr);return 77;
    }
    for(int i=1;i+1<argc;++i)if(!strcmp(argv[i],"--exec-path-child")) {
        char target[4096]={0};ssize_t size=readlink("/proc/self/exe",target,sizeof(target)-1);
        if(size<0 || strcmp(target,argv[i+1])){fprintf(stderr,"exec child exe mismatch: %s expected %s\n",target,argv[i+1]);_exit(1);}
        _exit(42);
    }
    if(getpid()!=1){fputs("Guest probe requires the selected PID1 init\n",stderr);return 77;}
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0){dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);executable_directory_probe();executable_identity_probe();return module_previous_probe();
}

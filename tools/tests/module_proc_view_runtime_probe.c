/* SPDX-License-Identifier: Apache-2.0 */
/* Selected PID1 only. No mount or module mutations are permitted on the host. */
#define MODULE_MOUNT_PROBE_ENTRY module_mount_previous_probe
#include "module_mount_runtime_probe.c"
#undef MODULE_MOUNT_PROBE_ENTRY

/** @brief Read a complete bounded generated view without altering its OFD offset.
 * @param fd Borrowed readable descriptor with captured proc state.
 * @param bytes Writable buffer, required.
 * @param capacity Bytes including a terminating NUL.
 * @return None; failed target-system assertions terminate the probe.
 */
static void read_proc_view(int fd,char *bytes,size_t capacity)
{
    ssize_t count=pread(fd,bytes,capacity-1,0);
    require(count>=0 && (size_t)count<capacity-1,"read bounded captured proc mount view");bytes[count]=0;
}

/** @brief Check an actual mountinfo point/type/source tuple.
 * @param text Borrowed complete mountinfo text.
 * @param wanted Expected decoded point without whitespace.
 * @param source Expected source without whitespace.
 * @return True for exactly one matching tmpfs record; no ownership transfer.
 */
static bool proc_view_has_point(const char *text,const char *wanted,const char *source)
{
    unsigned matches=0;
    for(const char *line=text;line && *line;line=strchr(line,'\n')) {
        if(*line=='\n')++line;if(!*line)break;
        char device[48],root[4096],point[4096],options[256];unsigned long long id,parent;
        if(sscanf(line,"%llu %llu %47s %4095s %4095s %255s",&id,&parent,device,root,point,options)!=6)continue;
        const char *separator=strstr(line," - "),*end=strchr(line,'\n');
        if(separator && (!end || separator<end) && !strcmp(point,wanted)) {
            char type[32],actual[64];
            if(sscanf(separator," - %31s %63s",type,actual)==2 && !strcmp(type,"tmpfs") && !strcmp(actual,source))++matches;
        }
    }
    return matches==1;
}

/** @brief Validate real open-time capture through chroot, SCM_RIGHTS and fork.
 * @return None. Owns only private tmpfs targets and selected-process descriptors;
 * all mounts and retained views are released before the module regression runs.
 */
static void proc_view_runtime_probe(void)
{
    const char *base="/dev/shm/ko-proc-view",*root="/dev/shm/ko-proc-view/root";
    const char *nested="/dev/shm/ko-proc-view/root/nested";
    require(!mkdir(base,0700) && !mkdir(root,0700),"reserve private proc view targets");
    require(!mount("ko-view-root",root,"tmpfs",0,"size=8192,mode=0700"),"mount proc capture root");
    int endpoints[2];require(!socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,endpoints),"create proc descriptor channel");
    pid_t target=fork();require(target>=0,"fork proc capture target");
    if(!target) {
        require(!close(endpoints[0]),"close unused target channel");
        int proc=open("/proc/self",O_RDONLY|O_DIRECTORY);require(proc>=0,"hold target proc directory across chroot");
        int files[2];files[0]=openat(proc,"mountinfo",O_RDONLY);require(files[0]>=0,"open target view before chroot");
        require(!chroot(root) && !chdir("/"),"change target root after first open");
        files[1]=openat(proc,"mountinfo",O_RDONLY);require(files[1]>=0,"open same proc inode after chroot");
        char marker='V';union {struct cmsghdr alignment;char bytes[CMSG_SPACE(sizeof(files))];} control={0};
        struct iovec data={&marker,1};struct msghdr message={.msg_iov=&data,.msg_iovlen=1,
            .msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
        struct cmsghdr *rights=CMSG_FIRSTHDR(&message);rights->cmsg_level=SOL_SOCKET;
        rights->cmsg_type=SCM_RIGHTS;rights->cmsg_len=CMSG_LEN(sizeof(files));memcpy(CMSG_DATA(rights),files,sizeof(files));
        require(sendmsg(endpoints[1],&message,MSG_NOSIGNAL)==1,"transfer captured proc OFDs");
        require(!close(files[0]) && !close(files[1]) && !close(proc) && !close(endpoints[1]),"release target-owned proc descriptors");
        _exit(0);
    }
    require(!close(endpoints[1]),"close unused receiver channel");
    char marker=0;union {struct cmsghdr alignment;char bytes[CMSG_SPACE(2*sizeof(int))];} control={0};
    struct iovec data={&marker,1};struct msghdr message={.msg_iov=&data,.msg_iovlen=1,
        .msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    require(recvmsg(endpoints[0],&message,0)==1 && marker=='V' && !(message.msg_flags&MSG_CTRUNC),"receive complete captured proc OFDs");
    struct cmsghdr *rights=CMSG_FIRSTHDR(&message);
    require(rights && rights->cmsg_level==SOL_SOCKET && rights->cmsg_type==SCM_RIGHTS &&
        rights->cmsg_len==CMSG_LEN(2*sizeof(int)),"validate captured proc rights");
    int files[2];memcpy(files,CMSG_DATA(rights),sizeof(files));require(!close(endpoints[0]),"close proc descriptor channel");
    int status=0;require(waitpid(target,&status,0)==target && WIFEXITED(status) && !WEXITSTATUS(status),"reap original captured-view target");
    char old_view[16384],new_view[16384];read_proc_view(files[0],old_view,sizeof(old_view));read_proc_view(files[1],new_view,sizeof(new_view));
    require(proc_view_has_point(old_view,root,"ko-view-root") && proc_view_has_point(new_view,"/","ko-view-root"),
        "same inode opens preserve distinct roots after target exit");
    require(!strstr(new_view," - proc ") && !strstr(new_view,base),"new captured root hides outside mounts");
    int alias=dup(files[0]);require(alias>=0 && !close(files[0]),"share captured state through dup after target exit");
    char prefix[32];require(lseek(alias,0,SEEK_SET)==0 && read(alias,prefix,sizeof(prefix))==(ssize_t)sizeof(prefix) &&
        !memcmp(prefix,old_view,sizeof(prefix)),"seek/read captured state after final original descriptor close");
    pid_t inherited=fork();require(inherited>=0,"fork inherited captured-view reader");
    if(!inherited) {
        char content[16384];read_proc_view(alias,content,sizeof(content));
        require(proc_view_has_point(content,root,"ko-view-root"),"fork shares original captured root");_exit(0);
    }
    require(waitpid(inherited,&status,0)==inherited && WIFEXITED(status) && !WEXITSTATUS(status),"reap inherited view reader");
    require(!mkdir(nested,0700) && !mount("ko-view-child",nested,"tmpfs",0,"size=8192,mode=0700"),"change live graph after captured target exit");
    read_proc_view(alias,old_view,sizeof(old_view));read_proc_view(files[1],new_view,sizeof(new_view));
    require(proc_view_has_point(old_view,nested,"ko-view-child") && proc_view_has_point(new_view,"/nested","ko-view-child"),
        "captured roots observe subsequent live namespace changes");
    require(!umount(nested),"retire private nested capture mount");
    errno=0;require(umount(root)==-1 && errno==EBUSY,"captured root reference prevents backend retirement");
    require(!close(files[1]) && !close(alias) && !umount(root),"last captured-view closes release backend references");
    require(!rmdir(root) && !rmdir(base),"remove only private proc view targets");
    puts("MODULE_RUNTIME_PROC_VIEW distinct open roots, chroot, target exit, SCM_RIGHTS, dup/fork, live graph and final provider release");
}

/** @brief Run only as ReliefNT PID1, then continue existing mount/module probes.
 * @return Host refusal 77 before mutators, or the existing module probe status.
 */
int main(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT") || getpid()!=1) {
        fputs("Guest-only proc view probe: refusing host mutations\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0){dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);puts("MODULE_RUNTIME_PROC_VIEW_READY");
    proc_view_runtime_probe();return module_mount_previous_probe();
}

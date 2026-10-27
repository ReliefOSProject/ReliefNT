/* SPDX-License-Identifier: Apache-2.0 */
/* Selected PID1 only. Mount and module mutations never run on the host. */
#define main module_previous_probe
#include "module_runtime_probe.c"
#undef main
#include <mntent.h>
#include <stdbool.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <dirent.h>
#ifndef MODULE_MOUNT_PROBE_ENTRY
#define MODULE_MOUNT_PROBE_ENTRY main
#endif

static void directory_records_probe(int directory,ino_t child_inode)
{
    struct stat root,data;
    require(!fstat(directory,&root) && !fstatat(directory,"data",&data,AT_SYMLINK_NOFOLLOW),"stat actual directory identities");
    DIR *stream=fdopendir(dup(directory));require(stream!=NULL,"open retained directory stream");
    struct dirent *entry=readdir(stream);require(entry!=NULL,"read first actual directory record");
    long cookie=telldir(stream);require(cookie>=0,"capture opaque directory cookie");
    entry=readdir(stream);require(entry!=NULL,"read second actual directory record");
    char second[256];snprintf(second,sizeof(second),"%s",entry->d_name);ino_t second_inode=entry->d_ino;
    seekdir(stream,cookie);entry=readdir(stream);
    require(entry && entry->d_ino==second_inode && !strcmp(entry->d_name,second),"seekdir restores exact backend cookie");
    rewinddir(stream);unsigned files=0,children=0;errno=0;
    while((entry=readdir(stream))) {
        if(!strcmp(entry->d_name,"data")) {
            require(entry->d_ino==data.st_ino && entry->d_type==DT_REG,"directory record reports actual regular inode and type");++files;
        } else if(!strcmp(entry->d_name,"child")) {
            require(entry->d_ino==child_inode && entry->d_type==DT_DIR,"directory record retains underlying overmounted point inode");++children;
        } else if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))
            require(entry->d_ino==root.st_ino && entry->d_type==DT_DIR,"dot records describe mounted filesystem root");
    }
    require(!errno && files==1 && children==1 && !closedir(stream),"enumeration reaches EOF once after rewind");
    int file=openat(directory,"data",O_RDONLY);require(file>=0,"open actual seek source");
    require(lseek(file,-1,SEEK_END)==9 && lseek(file,0,SEEK_DATA)==0 && lseek(file,0,SEEK_HOLE)==10,
        "seek reads actual provider size and valid conservative data/hole positions");
    errno=0;require(lseek(file,10,SEEK_DATA)==-1 && errno==ENXIO && !close(file),"seek data rejects EOF");
}

static unsigned long long mount_record(const char *wanted,const char *source,bool readonly,
                                          unsigned long long *parent)
{
    FILE *table=setmntent("/proc/self/mounts","r");require(table!=NULL,"open actual mounts view");
    struct mntent *entry;int found=0;
    while((entry=getmntent(table)))if(!strcmp(entry->mnt_dir,wanted)) {
        require(!strcmp(entry->mnt_type,"tmpfs") && !strcmp(entry->mnt_fsname,source),"mntent decodes actual mount source and point");
        require(hasmntopt(entry,readonly ? "ro" : "rw")!=NULL,"mounts reflects actual readonly policy");++found;
    }
    require(found==1 && endmntent(table)==1,"one actual mount record");
    table=fopen("/proc/self/mountinfo","r");require(table!=NULL,"open actual mountinfo view");
    char line[4096];unsigned long long id=0;
    while(fgets(line,sizeof(line),table)) {
        unsigned long long record=0,ancestor=0;char device[48],root[4096],point[4096];
        if(sscanf(line,"%llu %llu %47s %4095s %4095s",&record,&ancestor,device,root,point)==5 && !strcmp(point,wanted)) {
            require(!id && !strcmp(root,"/"),"mountinfo has one actual mounted filesystem root");
            id=record;if(parent)*parent=ancestor;
        }
    }
    require(id && !fclose(table),"read actual mount ID");return id;
}
static void mount_description_probe(void)
{
    const char *base="/dev/shm/ko-mount-qa",*from="/dev/shm/ko-mount-qa/a",*to="/dev/shm/ko-mount-qa/b";
    const char *nested="/dev/shm/ko-mount-qa/b/child";
    require(!mkdir(base,0700) && !mkdir(from,0700) && !mkdir(to,0700),"reserve private mount targets");
    require(!mount("runtime qa",from,"tmpfs",MS_NOSUID|MS_NODEV|MS_NOEXEC,"size=8192,nr_inodes=32,mode=0750"),"mount runtime owned tmpfs");
    int directory=open(from,O_RDONLY|O_DIRECTORY);require(directory>=0,"hold actual runtime root");
    int file=openat(directory,"data",O_CREAT|O_EXCL|O_RDWR,0600);
    require(file>=0 && write(file,"mount data",10)==10 && !close(file),"write private mounted inode");
    require(!mkdirat(directory,"child",0700),"create actual child mountpoint");
    struct stat child_status;require(!fstatat(directory,"child",&child_status,AT_SYMLINK_NOFOLLOW),"capture underlying mountpoint inode");
    char child_before[128];snprintf(child_before,sizeof(child_before),"%s/child",from);
    require(!mount("child",child_before,"tmpfs",0,"size=8192,mode=0700"),"mount real nested tmpfs");
    require(!mount(from,to,NULL,MS_MOVE,NULL),"move actual mount with held root and child");
    unsigned long long parent=0,root_id=mount_record(to,"runtime qa",false,NULL);
    require(mount_record(nested,"child",false,&parent)!=root_id && parent==root_id,"mountinfo parent survives subtree move");
    directory_records_probe(directory,child_status.st_ino);
    char bytes[16]={0};file=openat(directory,"data",O_RDONLY);
    require(file>=0 && read(file,bytes,sizeof(bytes))==10 && !memcmp(bytes,"mount data",10) && !close(file),
        "held dirfd still names moved filesystem");
    file=openat(directory,"data",O_RDONLY);require(file>=0,"open mapped actual inode");
    errno=0;
    require(mmap(NULL,4096,PROT_READ|PROT_EXEC,MAP_PRIVATE,file,0)==MAP_FAILED && errno==EPERM,
        "actual noexec mount rejects executable mapping after move");
    char *mapping=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,file,0);
    require(mapping!=MAP_FAILED && !memcmp(mapping,"mount data",10) && !close(file),
        "mapped provider stays alive after numeric descriptor close");
    require(!munmap(mapping,4096),"release mapped actual provider");
    errno=0;require(umount(to)==-1 && errno==EBUSY,"child and root references prevent parent unmount");
    require(!umount(nested),"unmount nested filesystem");
    require(!mount((const char *)(uintptr_t)1,to,(const char *)(uintptr_t)1,
        MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL),"readonly remount ignores source/type pointers");
    mount_record(to,"runtime qa",true,NULL);
    errno=0;file=openat(directory,"data",O_WRONLY);
    require(file==-1 && errno==EROFS,"readonly policy applies to moved held directory opens");
    file=openat(directory,"data",O_RDONLY);require(file>=0,"open readonly mapping source");
    mapping=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,file,0);
    require(mapping!=MAP_FAILED && mapping[0]=='m',"readonly filesystem permits private writable COW mapping");
    mapping[0]='X';require(!munmap(mapping,4096),"release private COW mapping");
    memset(bytes,0,sizeof(bytes));require(pread(file,bytes,10,0)==10 && !memcmp(bytes,"mount data",10) && !close(file),
        "private writable mapping preserves readonly source inode");
    require(!close(directory) && !chdir(to),"retain mount solely through actual cwd");
    errno=0;require(umount(to)==-1 && errno==EBUSY,"actual cwd prevents unmount");
    require(!chdir("/") && !umount(to),"release cwd then detach and retire runtime backend");
    char removed[128];snprintf(removed,sizeof(removed),"%s/data",to);
    errno=0;require(access(removed,F_OK)==-1 && errno==ENOENT,"underlying target returns after detach");
    for(unsigned cycle=0;cycle<64;++cycle) {
        require(!mount("reclaim",from,"tmpfs",0,"size=8192,nr_inodes=8,mode=0700"),"repeat mount after backend slot reclaim");
        require(!umount(from),"repeat unmount without scoped self-reference leak");
    }
    require(!rmdir(from) && !rmdir(to) && !rmdir(base),"remove only private mount targets");
    puts("MODULE_RUNTIME_MOUNTS actual graph, mntent, nested MS_MOVE, actual dirent identities/cookies, provider seek, held dirfd, readonly, cwd busy and64backend reclaims");
}
/** @brief Run the owned mount/module probe only as ReliefNT PID1.
 * @return Host refusal 77 before mutators, or the base module probe status.
 */
int MODULE_MOUNT_PROBE_ENTRY(void)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT") || getpid()!=1) {
        fputs("Guest-only mount probe: refusing host mutations\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0){dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);puts("MODULE_RUNTIME_MOUNTS_READY");
    mount_description_probe();return module_previous_probe();
}

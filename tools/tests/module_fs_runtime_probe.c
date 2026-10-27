/* SPDX-License-Identifier: Apache-2.0 */
/* Guest-only FAT namespace acceptance followed by the common module probe.
 * Use argv[1] to select a FAT mount; /boot is the default QA convention.
 * Run only as the explicitly selected PID1 test init. Linux hosts return77
 * before filesystem or module mutations. All writes use one newly made tree. */
#define main module_core_main
#include "module_runtime_probe.c"
#undef main
static void fat_namespace_probe(const char *mountpoint)
{
    struct statfs filesystem;
    if(statfs(mountpoint,&filesystem)) {
        require(errno==ENOENT,"read requested FAT test mount");
        puts("MODULE_RUNTIME_FAT_SKIP test mount absent");return;
    }
    printf("MODULE_RUNTIME_FAT_CHECK type=%lx flags=%lx\n",(unsigned long)filesystem.f_type,(unsigned long)filesystem.f_flags);
    if(filesystem.f_type!=0x4d44 || (filesystem.f_flags&1)) {
        puts("MODULE_RUNTIME_FAT_SKIP boot is not writable FAT");return;
    }
    require(!chdir(mountpoint),"enter writable FAT test mount");
    char mount_name[4096],expected_name[4096];
    require(getcwd(mount_name,sizeof(mount_name))!=NULL,"name FAT test mount");
    int expected_length=snprintf(expected_name,sizeof(expected_name),"%s%s.ko-fat-qa/right/moved-dir",
        mount_name,strcmp(mount_name,"/") ? "/" : "");
    require(expected_length>0 && (size_t)expected_length<sizeof(expected_name),"bound FAT test pathname");
    require(!mkdir(".ko-fat-qa",0700),"create private FAT namespace fixture");
    int mount=open(".",O_RDONLY|O_DIRECTORY);
    int root=open(".ko-fat-qa",O_RDONLY|O_DIRECTORY);
    require(mount>=0,"retain FAT test mount");
    require(root>=0 && !mkdirat(root,"left",0750) && !mkdirat(root,"right",0700),"create FAT parent directories");
    int left=openat(root,"left",O_RDONLY|O_DIRECTORY),right=openat(root,"right",O_RDONLY|O_DIRECTORY);
    require(left>=0 && right>=0,"retain FAT directories");
    int data=openat(left,"payload",O_RDWR|O_CREAT|O_EXCL,0600);
    require(data>=0 && write(data,"original",8)==8,"populate FAT held file");
    require(!fchmod(data,0640) && !fchown(data,123,456),"set FAT inode ownership");
    require(!renameat(left,"payload",right,"moved"),"FAT cross-directory file rename");
    struct stat information;
    require(!fstatat(right,"moved",&information,0) && (information.st_mode&07777)==0640 &&
        information.st_uid==123 && information.st_gid==456,"FAT moved ownership persists");
    require(!mkdirat(left,"directory",0750),"create FAT directory to move");
    int directory=openat(left,"directory",O_RDONLY|O_DIRECTORY);require(directory>=0,"retain FAT moved directory");
    for(unsigned i=0;i<50;++i) {
        char name[32];snprintf(name,sizeof(name),"file-%02u",i);
        int entry=openat(directory,name,O_WRONLY|O_CREAT|O_EXCL,0600);
        require(entry>=0 && !close(entry),"populate growing FAT directory");
    }
    require(!renameat(left,"directory",right,"moved-dir"),"FAT cross-directory nonempty directory rename");
    require(!fchdir(directory),"enter moved held FAT directory");
    char cwd[4096];
    require(syscall(SYS_getcwd,cwd,sizeof(cwd))>0 && !strcmp(cwd,expected_name),"FAT moved cwd graph");
    int parent=open("..",O_RDONLY|O_DIRECTORY);struct stat actual,expected;
    require(parent>=0 && !fstat(parent,&actual) && !fstat(right,&expected) && actual.st_ino==expected.st_ino &&
        actual.st_dev==expected.st_dev,"FAT moved dot-dot parent");
    require(!close(parent) && !chdir("/"),"leave moved FAT directory");
    for(unsigned i=0;i<50;++i) {
        char name[32];snprintf(name,sizeof(name),"file-%02u",i);
        require(!unlinkat(directory,name,0),"remove growing FAT directory members");
    }
    require(!unlinkat(right,"moved-dir",AT_REMOVEDIR) && !close(directory),"remove moved FAT directory");
    require(!unlinkat(right,"moved",0),"unlink held moved FAT file");
    char bytes[8];require(!fstat(data,&information) && !information.st_nlink &&
        pread(data,bytes,sizeof(bytes),0)==8 && !memcmp(bytes,"original",8),"unlinked moved FAT inode survives");
    require(!fallocate(data,0,8,8192) && !fstat(data,&information) && information.st_size==8200,
        "preallocate unlinked FAT inode by held object");
    require(!fsync(data) && !fdatasync(data) && !syncfs(data),"flush actual FAT backing after unlink");
    require(!close(data) && !close(left) && !close(right),"release FAT inode handles");
    require(!unlinkat(root,"left",AT_REMOVEDIR) && !unlinkat(root,"right",AT_REMOVEDIR) &&
        !close(root) && !unlinkat(mount,".ko-fat-qa",AT_REMOVEDIR) && !close(mount),"remove only private FAT test tree");
    puts("MODULE_RUNTIME_FAT cross-parent file/directory,50children,ownership,cwd,unlinked inode");
}
int main(int argc,char **argv)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT") || getpid()!=1) {
        fputs("Guest-only filesystem/module probe: refusing host mutations\n",stderr);return 77;
    }
    int serial=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
    if(serial>=0){dup2(serial,1);dup2(serial,2);if(serial>2)close(serial);}
    setvbuf(stdout,NULL,_IONBF,0);
    fat_namespace_probe(argc>1 ? argv[1] : "/boot");return module_core_main();
}

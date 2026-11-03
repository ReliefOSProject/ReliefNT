/* SPDX-License-Identifier: Apache-2.0 */
/* Portable POSIX namespace/permission checks plus repeatable stat workloads. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(expr) do { if(!(expr)) { \
    fprintf(stderr,"VFS_CACHE_FAIL line=%d errno=%d check=%s\n",__LINE__,errno,#expr);exit(1); \
} ++checks; } while(0)
static void absent(const char *path)
{struct stat s;errno=0;CHECK(stat(path,&s)==-1 && errno==ENOENT);}
static struct stat status(const char *path)
{struct stat s;CHECK(!stat(path,&s));return s;}
static int make_file(const char *name,const char *bytes)
{
    int fd=open(name,O_RDWR|O_CREAT|O_EXCL,0600);CHECK(fd>=0);
    CHECK(write(fd,bytes,strlen(bytes))==(ssize_t)strlen(bytes));return fd;
}
static uint64_t now(void)
{struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}
static void benchmark(const char *base)
{
    const unsigned iterations=10000;
    for(unsigned round=0;round<3;++round) {
        for(unsigned negative=0;negative<2;++negative) {
            struct stat s;const char *name=negative ? "missing-bench" : "bench";
            uint64_t start=now();
            for(unsigned i=0;i<iterations;++i) {
                int rc=stat(name,&s);
                if(negative)CHECK(rc==-1 && errno==ENOENT);else CHECK(rc==0 && s.st_size==7);
            }
            uint64_t elapsed=now()-start;
            printf("VFS_CACHE_BENCH base=%s kind=%s round=%u iterations=%u ns=%" PRIu64 "\n",
                   base,negative ? "missing" : "present",round,iterations,elapsed);
        }
    }
}
int main(int argc,char **argv)
{
    CHECK(argc==2);char directory[4096];
    CHECK(snprintf(directory,sizeof(directory),"%s/vfs-cache-XXXXXX",argv[1])<(int)sizeof(directory));
    CHECK(mkdtemp(directory)!=NULL);CHECK(!chdir(directory));CHECK(!chmod(".",0755));
    absent("file");absent("file");int old=make_file("file","abcdef");
    CHECK(status("file").st_size==6);CHECK(status("file").st_size==6);
    errno=0;CHECK(open("file",O_CREAT|O_EXCL|O_WRONLY,0600)==-1 && errno==EEXIST);
    CHECK(!ftruncate(old,2));CHECK(status("file").st_size==2);
    absent("alias");CHECK(!link("file","alias"));
    struct stat first=status("file"),alias=status("alias"),held;
    CHECK(first.st_ino==alias.st_ino && alias.st_nlink==2);
    CHECK(!unlink("file"));absent("file");CHECK(!fstat(old,&held) && held.st_nlink==1);
    CHECK(!unlink("alias"));absent("alias");CHECK(!fstat(old,&held) && held.st_nlink==0);
    int fresh=make_file("file","new");CHECK(status("file").st_ino!=held.st_ino);
    char bytes[8]={0};CHECK(lseek(old,0,SEEK_SET)==0);CHECK(read(old,bytes,sizeof(bytes))==2);
    CHECK(!memcmp(bytes,"ab",2));CHECK(!close(old));
    absent("shortcut");CHECK(!symlink("file","shortcut"));
    CHECK(status("shortcut").st_size==3);struct stat linkstat;
    CHECK(!lstat("shortcut",&linkstat) && S_ISLNK(linkstat.st_mode));
    memset(bytes,0,sizeof(bytes));CHECK(readlink("shortcut",bytes,sizeof(bytes))==4);
    CHECK(!memcmp(bytes,"file",4));
    int replacement=make_file("replacement","updated");
    CHECK(!rename("replacement","file"));absent("replacement");
    CHECK(status("shortcut").st_size==7);CHECK(!fstat(fresh,&held) && held.st_size==3 && !held.st_nlink);
    CHECK(!close(fresh));CHECK(!close(replacement));CHECK(!unlink("shortcut"));
    CHECK(!mkdir("dir",0755));int child=make_file("dir/child","x");CHECK(!close(child));
    int dirfd=open("dir",O_RDONLY|O_DIRECTORY);CHECK(dirfd>=0);
    struct stat childstat=status("dir/child");absent("moved");CHECK(!rename("dir","moved"));
    absent("dir");CHECK(!fstatat(dirfd,"child",&held,0) && held.st_ino==childstat.st_ino);
    CHECK(!unlink("moved/child"));CHECK(!close(dirfd));CHECK(!rmdir("moved"));
    if(geteuid()==0) {
        CHECK(!mkdir("private",0700));int fd=make_file("private/secret","s");CHECK(!close(fd));
        CHECK(status("private/secret").st_size==1);absent("private/missing");
        CHECK(!seteuid(65534));errno=0;
        CHECK(stat("private/secret",&held)==-1 && errno==EACCES);errno=0;
        CHECK(stat("private/missing",&held)==-1 && errno==EACCES);
        CHECK(!seteuid(0));CHECK(!chmod("private",0755));CHECK(!seteuid(65534));
        absent("private/missing");CHECK(!seteuid(0));
        CHECK(!unlink("private/secret"));CHECK(!rmdir("private"));
    }
    int bench=make_file("bench","payload");CHECK(!close(bench));benchmark(argv[1]);
    CHECK(!unlink("bench"));CHECK(!unlink("file"));CHECK(!chdir("/"));CHECK(!rmdir(directory));
    printf("VFS_CACHE_PASS base=%s checks=%u\n",argv[1],checks);return 0;
}

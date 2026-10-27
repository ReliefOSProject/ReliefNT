/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

/* Private sparse regular file only: no device or module syscalls. This is a
 * Linux x86_64 reference for the register ABI, not ReliefNT runtime evidence. */
int main(void)
{
    const char *temporary=getenv("TMPDIR");assert(temporary && *temporary=='/');
    char path[4096];int length=snprintf(path,sizeof(path),"%s/vector-offset-XXXXXX",temporary);
    assert(length>0 && (size_t)length<sizeof(path));
    int fd=mkstemp(path);assert(fd>=0 && !unlink(path));
    const uint64_t position=UINT64_C(0x100000007),ignored=UINT64_C(0xfedcba9876543210);
    char bytes[]="abcdef",got[6]={0};
    struct iovec output[]={{bytes,2},{bytes+2,4}},input[]={{got,3},{got+3,3}};
    assert(lseek(fd,21,SEEK_SET)==21);
    assert(syscall(SYS_pwritev,fd,output,2,position,ignored)==6);
    struct stat status;assert(!fstat(fd,&status) && (uint64_t)status.st_size==UINT64_C(0x10000000d));
    assert(syscall(SYS_preadv,fd,input,2,position,ignored)==6 && !memcmp(got,"abcdef",6));
    assert(lseek(fd,0,SEEK_CUR)==21);
    assert(syscall(SYS_pwritev2,fd,output,2,position,ignored,UINT64_C(0x100000000))==6);
    memset(got,0,sizeof(got));
    assert(syscall(SYS_preadv2,fd,input,2,position,ignored,UINT64_C(0x100000000))==6 && !memcmp(got,"abcdef",6));
    assert(lseek(fd,0,SEEK_CUR)==21);
    errno=0;assert(syscall(SYS_preadv,fd,input,2,UINT64_MAX,ignored)==-1 && errno==EINVAL);
    assert(lseek(fd,(off_t)position,SEEK_SET)==(off_t)position);
    memset(got,0,sizeof(got));
    assert(syscall(SYS_preadv2,fd,input,2,UINT64_MAX,ignored,0)==6 && !memcmp(got,"abcdef",6));
    assert(lseek(fd,0,SEEK_CUR)==(off_t)UINT64_C(0x10000000d));
    assert(lseek(fd,(off_t)position,SEEK_SET)==(off_t)position);
    assert(syscall(SYS_pwritev2,fd,output,2,UINT64_MAX,ignored,0)==6);
    assert(lseek(fd,0,SEEK_CUR)==(off_t)UINT64_C(0x10000000d));
    assert(!ftruncate(fd,6) && lseek(fd,2,SEEK_SET)==2);
    assert(syscall(SYS_preadv2,fd,input,2,UINT64_C(0xffffffff),0,0)==0);
    assert(lseek(fd,0,SEEK_CUR)==2 && !close(fd));
    puts("PASS Linux x86_64 raw vector position: full64, unused pos_h, v2 -1, flags low32 and sparse >4GiB");
    return 0;
}

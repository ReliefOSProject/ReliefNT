/* SPDX-License-Identifier: Apache-2.0 */
/* Native private guest: reject invalid files, then load/unload the valid module. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

/** @brief Load a private negative fixture and require the exact loader error.
 * @param path Read-only guest fixture. @param label Stable diagnostic label.
 * @return Zero only for ENOEXEC; caller has already admitted the owned guest.
 */
static int reject(const char *path, const char *label)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0)return 1;
    errno=0;long result=syscall(SYS_finit_module,fd,"",0);int saved=errno;
    close(fd);
    printf("MODULE_REJECTION %s result=%ld errno=%d\n",label,result,saved);
    return result!=-1 || saved!=ENOEXEC;
}

/** @brief Verify malformed ELF and wrong vermagic fail without poisoning loader state.
 * @return 77 before any module syscall outside an admitted private ReliefNT
 * root session; zero only after both rejections and a valid load/unload.
 * argv names three exclusively staged /run files and the exact expected release.
 */
int main(int argc,char **argv)
{
    struct utsname u;
    if(uname(&u) || strcmp(u.sysname,"ReliefNT") || getpid()==1 || geteuid()!=0) {
        fputs("Guest-only module rejection probe: refusing host mutations\n",stderr);return 77;
    }
    static const char expected[]="ReliefNT owned module rejection QA\n";
    char marker[sizeof(expected)];int owned=open("/run/qa-module-owned",O_RDONLY|O_CLOEXEC);
    ssize_t count=owned<0?-1:read(owned,marker,sizeof(marker));if(owned>=0)close(owned);
    if(count!=sizeof(expected)-1 || memcmp(marker,expected,sizeof(expected)-1) ||
       argc!=5 || strcmp(u.release,argv[4]))return 77;
    if(reject(argv[2],"truncated-ELF") || reject(argv[3],"wrong-vermagic"))return 1;
    int fd=open(argv[1],O_RDONLY|O_CLOEXEC);if(fd<0)return 1;
    long result=syscall(SYS_finit_module,fd,"",0);int saved=errno;close(fd);
    if(result) {printf("FAIL MODULE_REJECTION valid-load errno=%d\n",saved);return 1;}
    if(syscall(SYS_delete_module,"weak_import",O_NONBLOCK)) {
        printf("FAIL MODULE_REJECTION valid-unload errno=%d\n",errno);return 1;
    }
    puts("PASS MODULE_REJECTION truncated ELF and wrong vermagic rejected; valid load/unload still works");
    return 0;
}

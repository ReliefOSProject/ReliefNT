/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PARAMETER_RUNTIME_CHECKS_H
#define RELIEFNT_PARAMETER_RUNTIME_CHECKS_H
#include <sys/syscall.h>
#include <sys/wait.h>

/** @brief Read and compare one actual parameter stream in the owned guest.
 * @param name Borrowed absolute sysfs path.
 * @param expected Expected newline-terminated bytes.
 * @return None; require stops the already admitted private VM on failure.
 */
static void parameter_expect(const char *name,const char *expected)
{
    char value[1100]={0};int fd=open(name,O_RDONLY|O_CLOEXEC);require(fd>=0,"open real parameter");
    ssize_t got=read(fd,value,sizeof(value));require(got==(ssize_t)strlen(expected) && !memcmp(value,expected,(size_t)got),"exact parameter value");
    require(!close(fd),"close parameter");
}
/** @brief Load the exact matching native parameter fixture through finit_module.
 * @return None; all mutations occur after boot probe's OS/PID1/owned-root guard.
 */
static void parameter_load(void)
{
    int fd=open("/standalone.ko",O_RDONLY|O_CLOEXEC);require(fd>=0,"open matching parameters module");
    require(!syscall(SYS_finit_module,fd,"enabled signed_value=-2147483648 unsigned_value=0xffffffff label=\"hello world\"",0),"real parameter module load");
    require(!close(fd),"close module image");
}
/** @brief Exercise actual sysfs/OFD/DAC/store/unload/reload parameter lifetime.
 * @return None; caller already admitted the owned native guest. Uses only
 * Linux syscalls and POSIX descriptors, with no host mutation entrypoint.
 */
static void parameter_runtime_checks(void)
{
    static const char enabled[]="/sys/module/parameters/parameters/enabled";
    static const char label[]="/sys/module/parameters/parameters/runtime_label";
    static const char number[]="/sys/module/parameters/parameters/signed_value";
    parameter_load();struct stat before,after;
    require(!stat(enabled,&before) && (before.st_mode&07777)==0644 && !before.st_uid && !before.st_gid,"declared parameter permissions");
    parameter_expect(enabled,"Y\n");parameter_expect(number,"-2147483648\n");parameter_expect(label,"initial\n");
    int fd=open(enabled,O_WRONLY|O_TRUNC|O_CLOEXEC);require(fd>=0 && write(fd,"off\n",4)==4 && !close(fd),"shell-style O_TRUNC parameter store");
    parameter_expect(enabled,"N\n");
    fd=open(number,O_WRONLY|O_TRUNC|O_CLOEXEC);require(fd<0 && errno==EIO,"readonly setter rejected even for root");
    struct stat hidden;require(stat("/sys/module/parameters/parameters/label",&hidden)<0 && errno==ENOENT,"permissions-zero parameter is absent");
    int held=open(label,O_RDWR|O_CLOEXEC);require(held>=0 && write(held,"changed",7)==7,"writable charp runtime allocation");
    parameter_expect(label,"changed\n");
    char long_value[1025];memset(long_value,'x',sizeof(long_value));
    require(write(held,long_value,sizeof(long_value))<0 && errno==ENOSPC,"charp standard length limit");
    parameter_expect(label,"changed\n");
    pid_t child=fork();require(child>=0,"fork DAC child");
    if(!child) {
        if(setgid(1000) || setuid(1000))_exit(31);
        int denied=open(enabled,O_WRONLY);
        if(denied>=0)_exit(32);
        _exit(errno==EACCES ? 0 : 33);
    }
    int status;require(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"unprivileged parameter write DAC");
    require(!syscall(SYS_delete_module,"parameters",0),"metadata OFD does not pin unloadable code");
    require(read(held,long_value,1)<0 && errno==ENOENT,"old parameter OFD retired after unload");
    parameter_load();require(!stat(label,&after) && before.st_dev==after.st_dev,"reload new parameter instance");
    require(write(held,"wrong",5)<0 && errno==ENOENT,"old parameter OFD cannot store into reload");
    parameter_expect(label,"initial\n");
    require(!close(held) && !syscall(SYS_delete_module,"parameters",0),"final parameter cleanup");
    puts("PASS MODULE_PARAMETERS actual sysfs modes/DAC/short stores, charp bounds, unload and stale OFD reload isolation");
}
#endif

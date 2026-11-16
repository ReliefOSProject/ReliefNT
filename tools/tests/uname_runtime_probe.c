/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <reliefos/prctl.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "[uname-probe] FAIL line=%d %s errno=%d\n", \
            __LINE__, #condition, errno); exit(1); } } while (0)

static int selected;

static void set_mode(int mode)
{
    CHECK(prctl(RELIEFOS_PR_SET_UNAME_LINUX, (unsigned long)mode, 0UL, 0UL, 0UL) == 0);
}

static void check_mode(int mode)
{
    struct utsname info, raw;
    CHECK(prctl(RELIEFOS_PR_GET_UNAME_LINUX, 0UL, 0UL, 0UL, 0UL) == mode);
    CHECK(uname(&info) == 0);
    CHECK(syscall(SYS_uname, &raw) == 0);
    CHECK(!strcmp(info.sysname, mode ? "Linux" : "ReliefNT"));
    CHECK(!memcmp(&info, &raw, sizeof(info)));
}

static void *thread_worker(void *unused)
{
    (void)unused;
    check_mode(selected);
    set_mode(!selected);
    check_mode(!selected);
    return NULL;
}

static void wait_ok(pid_t pid)
{
    int status;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int exec_check = argc == 3 && !strcmp(argv[1], "--exec-check");
    const char *name = exec_check ? argv[2] : argc == 2 ? argv[1] : "ReliefNT";
    CHECK(!strcmp(name, "Linux") || !strcmp(name, "ReliefNT"));
    selected = !strcmp(name, "Linux");
    check_mode(selected);
    if (exec_check) return 0;

    struct utsname original, changed;
    CHECK(uname(&original) == 0);
    set_mode(!selected);
    check_mode(!selected);
    CHECK(uname(&changed) == 0);
    /* Only sysname may change, including its terminating/unused bytes. */
    memset(original.sysname, 0, sizeof(original.sysname));
    memset(changed.sysname, 0, sizeof(changed.sysname));
    CHECK(!memcmp(&original, &changed, sizeof(original)));
    set_mode(selected);
    errno = 0;
    CHECK(prctl(RELIEFOS_PR_SET_UNAME_LINUX, 2UL, 0UL, 0UL, 0UL) == -1 && errno == EINVAL);
    CHECK(prctl(RELIEFOS_PR_SET_UNAME_LINUX, 1UL << 32, 0UL, 0UL, 0UL) == -1 && errno == EINVAL);
    for (int i = 0; i < 4; ++i) {
        unsigned long reserved[4] = {0};
        reserved[i] = 1;
        CHECK(prctl(RELIEFOS_PR_GET_UNAME_LINUX, reserved[0], reserved[1],
                    reserved[2], reserved[3]) == -1 && errno == EINVAL);
        if (i) CHECK(prctl(RELIEFOS_PR_SET_UNAME_LINUX, (unsigned long)!selected,
                           reserved[1], reserved[2], reserved[3]) == -1 && errno == EINVAL);
        check_mode(selected);
    }

    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        check_mode(selected);
        set_mode(!selected);
        check_mode(!selected);
        _exit(0);
    }
    wait_ok(child);
    check_mode(selected);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, thread_worker, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    check_mode(selected);

    char *arguments[] = {argv[0], "--exec-check", (char *)name, NULL};
    child = fork();
    CHECK(child >= 0);
    if (!child) { execv(argv[0], arguments); _exit(111); }
    wait_ok(child);
    child = vfork();
    if (!child) { execv(argv[0], arguments); _exit(112); }
    wait_ok(child);
    check_mode(selected);

    if (geteuid() == 0) {
        child = fork();
        CHECK(child >= 0);
        if (!child) {
            CHECK(setgid(1000) == 0 && setuid(1000) == 0);
            set_mode(!selected);
            check_mode(!selected);
            _exit(0);
        }
        wait_ok(child);
        check_mode(selected);
    }
    printf("[uname-probe] PASS sysname=%s libc/raw, fields, validation, fork/clone/exec/vfork, isolation\n", name);
    return 0;
}

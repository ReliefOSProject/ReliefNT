/* Runtime contract probe for the ReliefOS primary DRM node.
 * The present-device cases exercise the VirtIO-GPU PCI device and the
 * shared DRM file operations. See virtgpu_guest.c for scanout and PRIME.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/virtgpu_drm.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>

#define LINUX_RWF_HIPRI 0x00000001u

static uint32_t linux_major(uint64_t dev)
{
    return (uint32_t)((dev >> 8) & 0xfffU) |
           (uint32_t)((dev >> 32) & 0xfffff000U);
}

static uint32_t linux_minor(uint64_t dev)
{
    return (uint32_t)(dev & 0xffU) |
           (uint32_t)((dev >> 12) & 0xffffff00U);
}

static volatile sig_atomic_t interrupt_alarm_received;

static void interrupt_alarm_handler(int signal_number)
{
    (void)signal_number;
    interrupt_alarm_received = 1;
}

static void sysfs_step(const char *name)
{
    printf("DRM_SYSFS_STEP %s\n", name);
    fflush(stdout);
}

static int read_attribute(const char *name, const char *path,
                          char *buffer, size_t capacity)
{
    int fd;
    ssize_t count;
    char step[64];
    if (!capacity) return -1;
    snprintf(step, sizeof(step), "%s.open.begin", name);
    sysfs_step(step);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    snprintf(step, sizeof(step), "%s.open.returned", name);
    sysfs_step(step);
    if (fd < 0) return -1;
    snprintf(step, sizeof(step), "%s.read.begin", name);
    sysfs_step(step);
    count = read(fd, buffer, capacity - 1);
    snprintf(step, sizeof(step), "%s.read.returned", name);
    sysfs_step(step);
    int saved_errno = errno;
    close(fd);
    snprintf(step, sizeof(step), "%s.close.returned", name);
    sysfs_step(step);
    if (count < 0) {
        errno = saved_errno;
        return -1;
    }
    buffer[count] = 0;
    return (int)count;
}

static int check_sysfs_identity(int fd)
{
    struct stat st;
    int32_t rendering = -1;
    struct drm_virtgpu_getparam query = {
        .param = VIRTGPU_PARAM_3D_FEATURES,
        .value = (uintptr_t)&rendering,
    };
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &query) < 0 ||
        (rendering != 0 && rendering != 1))
        return 10;
    char link[32] = {0};
    char subsystem[128] = {0};
    char uevent[512] = {0};
    char device_number[32] = {0};
    sysfs_step("class-device.readlink.begin");
    int link_length = readlink("/sys/class/drm/card0/device", link,
                               sizeof(link) - 1);
    sysfs_step("class-device.readlink.returned");
    if (link_length < 0) return 1;
    link[link_length] = 0;
    if (strcmp(link, "../..")) return 2;
    sysfs_step("pci-subsystem.readlink.begin");
    int subsystem_length = readlink("/sys/dev/char/226:0/device/subsystem",
                                    subsystem, sizeof(subsystem) - 1);
    sysfs_step("pci-subsystem.readlink.returned");
    if (subsystem_length < 0) return 8;
    subsystem[subsystem_length] = 0;
    const char *subsystem_name = strrchr(subsystem, '/');
    if (!subsystem_name || strcmp(subsystem_name + 1, "pci")) return 9;
    sysfs_step("class-card.stat.begin");
    if (stat("/sys/class/drm/card0", &st) < 0 || !S_ISDIR(st.st_mode))
        return 3;
    sysfs_step("class-card.stat.returned");
    if (read_attribute("uevent", "/sys/dev/char/226:0/device/uevent",
                       uevent, sizeof(uevent)) < 0)
        return 4;
    if (!strstr(uevent, "PCI_ID=1AF4:1050\n") ||
        !strstr(uevent, "PCI_SLOT_NAME=0000:") ||
        !strstr(uevent, "MODALIAS=pci:v00001AF4d00001050"))
        return 5;
    if (read_attribute("drm-dev", "/sys/dev/char/226:0/device/drm/card0/dev",
                       device_number, sizeof(device_number)) < 0 ||
        strcmp(device_number, "226:0\n"))
        return 6;
    errno = 0;
    sysfs_step("render-stat.begin");
    int render_stat = stat("/dev/dri/renderD128", &st);
    if (rendering) {
        if (render_stat < 0 || !S_ISCHR(st.st_mode) ||
            linux_major(st.st_rdev) != 226 || linux_minor(st.st_rdev) != 128)
            return 7;
        if (read_attribute("render-dev", "/sys/class/drm/renderD128/dev",
                           device_number, sizeof(device_number)) < 0 ||
            strcmp(device_number, "226:128\n"))
            return 11;
        int render = open("/dev/dri/renderD128", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (render < 0) return 12;
        int valid = !fstat(render, &st) && S_ISCHR(st.st_mode) &&
                    linux_major(st.st_rdev) == 226 && linux_minor(st.st_rdev) == 128;
        int closed = close(render);
        if (!valid || closed) return 12;
    } else if (!render_stat || errno != ENOENT) {
        return 7;
    }
    sysfs_step("render-stat.returned");
    printf("DRM_SYSFS device=%s PCI_ID=1AF4:1050 primary=226:0 render=%s\n",
           link, rendering ? "226:128" : "absent");
    return 0;
}

static int check_open_file_lifetime(int fd)
{
    struct drm_auth original_magic = {0}, duplicate_magic = {0};
    struct drm_auth independent_magic = {0};
    struct drm_version version = {0};
    char name[16] = {0};
    int duplicate;
    int independent = -1;
    int result = 0;

    sysfs_step("lifetime.dup.begin");
    duplicate = dup(fd);
    sysfs_step("lifetime.dup.returned");
    if (duplicate < 0) {
        sysfs_step("lifetime.close-original.begin");
        close(fd);
        sysfs_step("lifetime.close-original.returned");
        return 1;
    }
    sysfs_step("lifetime.independent-open.begin");
    independent = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    sysfs_step("lifetime.independent-open.returned");
    if (independent < 0) {
        result = 2;
        goto out;
    }
    sysfs_step("lifetime.magic-original.begin");
    int original_result = ioctl(fd, DRM_IOCTL_GET_MAGIC, &original_magic);
    sysfs_step("lifetime.magic-original.returned");
    sysfs_step("lifetime.magic-duplicate.begin");
    int duplicate_result = ioctl(duplicate, DRM_IOCTL_GET_MAGIC, &duplicate_magic);
    sysfs_step("lifetime.magic-duplicate.returned");
    sysfs_step("lifetime.magic-independent.begin");
    int independent_result = ioctl(independent, DRM_IOCTL_GET_MAGIC, &independent_magic);
    sysfs_step("lifetime.magic-independent.returned");
    if (original_result < 0 || duplicate_result < 0 || independent_result < 0) {
        result = 3;
        goto out;
    }
    if (!original_magic.magic || original_magic.magic != duplicate_magic.magic ||
        original_magic.magic == independent_magic.magic) {
        result = 4;
        goto out;
    }
    sysfs_step("lifetime.close-original.begin");
    close(fd);
    sysfs_step("lifetime.close-original.returned");
    fd = -1;
    version.name = name;
    version.name_len = sizeof(name);
    sysfs_step("lifetime.survivor-version.begin");
    int survivor_version_result = ioctl(duplicate, DRM_IOCTL_VERSION, &version);
    sysfs_step("lifetime.survivor-version.returned");
    if (survivor_version_result < 0 || strcmp(name, "virtio_gpu"))
        result = 5;
    printf("DRM_OFD duplicate_magic=%u independent_magic=%u survivor=%s\n",
           original_magic.magic, independent_magic.magic,
           result ? "failed" : "usable");
out:
    if (fd >= 0) {
        sysfs_step("lifetime.final-close-original.begin");
        close(fd);
        sysfs_step("lifetime.final-close-original.returned");
    }
    if (duplicate >= 0) {
        sysfs_step("lifetime.final-close-duplicate.begin");
        close(duplicate);
        sysfs_step("lifetime.final-close-duplicate.returned");
    }
    if (independent >= 0) {
        sysfs_step("lifetime.final-close-independent.begin");
        close(independent);
        sysfs_step("lifetime.final-close-independent.returned");
    }
    return result;
}

static int check_positional_read(int fd, const char *operation)
{
    char event[sizeof(struct drm_event)] = {0};
    struct iovec iov = {.iov_base = event, .iov_len = sizeof(event)};
    ssize_t result;
    int saved_errno;
    int expected_errno = EAGAIN;
    char marker[64];

    snprintf(marker, sizeof(marker), "positional.%s.begin", operation);
    sysfs_step(marker);
    errno = 0;
    if (!strcmp(operation, "pread64")) {
        result = pread(fd, event, sizeof(event), 4096);
    } else if (!strcmp(operation, "preadv")) {
        result = preadv(fd, &iov, 1, 4096);
    } else if (!strcmp(operation, "preadv2")) {
        result = preadv2(fd, &iov, 1, 4096, 0);
    } else if (!strcmp(operation, "preadv2-current")) {
        result = preadv2(fd, &iov, 1, -1, 0);
    } else if (!strcmp(operation, "preadv2-hipri")) {
        result = preadv2(fd, &iov, 1, 4096, LINUX_RWF_HIPRI);
    } else if (!strcmp(operation, "preadv2-current-hipri")) {
        result = preadv2(fd, &iov, 1, -1, LINUX_RWF_HIPRI);
    } else if (!strcmp(operation, "preadv-zero-negative")) {
        expected_errno = EINVAL;
        result = preadv(fd, &iov, 0, -1);
    } else if (!strcmp(operation, "preadv2-zero-negative")) {
        expected_errno = EINVAL;
        result = preadv2(fd, &iov, 0, -2, 0);
    } else {
        return 64;
    }
    saved_errno = errno;
    snprintf(marker, sizeof(marker), "positional.%s.returned", operation);
    sysfs_step(marker);
    printf("DRM_POSITIONAL operation=%s result=%ld errno=%d\n",
           operation, (long)result, saved_errno);
    return result == -1 && saved_errno == expected_errno ? 0 : 1;
}

static int check_noop_lseek(int fd)
{
    errno = 0;
    sysfs_step("noop-lseek-set.begin");
    off_t set_result = lseek(fd, 1234, SEEK_SET);
    int set_errno = errno;
    sysfs_step("noop-lseek-set.returned");
    if (set_result != 0) return 1;

    errno = 0;
    sysfs_step("noop-lseek-hole.begin");
    off_t hole_result = lseek(fd, -1, 4); /* Linux SEEK_HOLE / SEEK_MAX. */
    int hole_errno = errno;
    sysfs_step("noop-lseek-hole.returned");
    if (hole_result != 0) return 2;

    errno = 0;
    sysfs_step("noop-lseek-invalid.begin");
    off_t invalid_result = lseek(fd, 0, 5);
    int invalid_errno = errno;
    sysfs_step("noop-lseek-invalid.returned");
    if (invalid_result != -1 || invalid_errno != EINVAL) return 3;
    printf("DRM_LSEEK set=%ld hole=%ld invalid=%d errno=%d\n",
           (long)set_result, (long)hole_result, (int)invalid_result,
           invalid_errno);
    (void)set_errno;
    (void)hole_errno;
    return 0;
}

static int check_interruptible_read(int fd)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    char event[sizeof(struct drm_event)];
    int original_flags;
    int changed_flags = 0;
    int installed_action = 0;
    int result_code = 1;

    sysfs_step("interruptible-read.getfl.begin");
    original_flags = fcntl(fd, F_GETFL);
    sysfs_step("interruptible-read.getfl.returned");
    if (original_flags < 0) return 1;
    action.sa_handler = interrupt_alarm_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; /* In particular, do not request SA_RESTART. */
    sysfs_step("interruptible-read.sigaction.begin");
    if (sigaction(SIGALRM, &action, &previous_action) < 0) {
        sysfs_step("interruptible-read.sigaction.failed");
        return 2;
    }
    installed_action = 1;
    sysfs_step("interruptible-read.sigaction.returned");
    sysfs_step("interruptible-read.blocking-flags.begin");
    if (fcntl(fd, F_SETFL, original_flags & ~O_NONBLOCK) < 0) {
        sysfs_step("interruptible-read.blocking-flags.failed");
        result_code = 3;
        goto cleanup;
    }
    changed_flags = 1;
    interrupt_alarm_received = 0;
    sysfs_step("interruptible-read.read.begin");
    alarm(1);
    errno = 0;
    ssize_t read_result = read(fd, event, sizeof(event));
    int read_error = errno;
    sysfs_step("interruptible-read.read.returned");
    alarm(0);
    printf("DRM_INTERRUPTIBLE_READ result=%ld errno=%d alarm=%d\n",
           (long)read_result, read_error, (int)interrupt_alarm_received);
    result_code = read_result == -1 && read_error == EINTR &&
                  interrupt_alarm_received ? 0 : 4;

cleanup:
    alarm(0);
    if (changed_flags && fcntl(fd, F_SETFL, original_flags) < 0)
        result_code = 5;
    if (installed_action && sigaction(SIGALRM, &previous_action, NULL) < 0)
        result_code = 6;
    return result_code;
}

static int run_positional_probe(const char *operation)
{
    int fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    int result;
    if (fd < 0) {
        printf("DRM_POSITIONAL open failed errno=%d\n", errno);
        return 10;
    }
    if (!strcmp(operation, "lseek")) result = check_noop_lseek(fd);
    else if (!strcmp(operation, "interruptible"))
        result = check_interruptible_read(fd);
    else result = check_positional_read(fd, operation);
    close(fd);
    return result;
}

static int expect_absent(void)
{
    struct stat st;
    int stat_result = stat("/dev/dri/card0", &st);
    int stat_error = errno;
    int fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    int open_error = errno;

    printf("DRM_RED stat=%d errno=%d open=%d errno=%d\n",
           stat_result, stat_error, fd, open_error);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    if (stat_result == 0 || (stat_error != ENOENT && stat_error != ENODEV) ||
        (open_error != ENOENT && open_error != ENODEV))
        return 2;
    return 0;
}

static int expect_present(void)
{
    struct stat st;
    struct drm_version version = {0};
    struct drm_get_cap cap = {0};
    char name[32] = {0};
    int fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        printf("DRM_PRESENT open failed errno=%d\n", errno);
        return 10;
    }
    if (fstat(fd, &st) < 0) {
        printf("DRM_PRESENT fstat failed errno=%d\n", errno);
        close(fd);
        return 11;
    }
    printf("DRM_PRESENT mode=%o major=%u minor=%u\n",
           (unsigned)(st.st_mode & S_IFMT), linux_major(st.st_rdev),
           linux_minor(st.st_rdev));
    if (!S_ISCHR(st.st_mode) || linux_major(st.st_rdev) != 226 ||
        linux_minor(st.st_rdev) != 0) {
        close(fd);
        return 12;
    }
    int sysfs_result = check_sysfs_identity(fd);
    if (sysfs_result) {
        printf("DRM_PRESENT sysfs identity check failed step=%d errno=%d\n",
               sysfs_result, errno);
        close(fd);
        return 19;
    }

    version.name = name;
    version.name_len = sizeof(name);
    sysfs_step("version.ioctl.begin");
    int version_result = ioctl(fd, DRM_IOCTL_VERSION, &version);
    sysfs_step("version.ioctl.returned");
    if (version_result < 0) {
        printf("DRM_PRESENT version failed errno=%d\n", errno);
        close(fd);
        return 13;
    }
    printf("DRM_VERSION name=%s len=%lu version=%d.%d.%d\n", name,
           (unsigned long)version.name_len, version.version_major,
           version.version_minor, version.version_patchlevel);
    if (strcmp(name, "virtio_gpu") || version.version_major != 0 ||
        version.version_minor != 1 || version.version_patchlevel != 0) {
        close(fd);
        return 14;
    }

    /* virtgpu_guest.c exercises actual import/export on the device. */
    cap.capability = DRM_CAP_PRIME;
    sysfs_step("cap-prime.ioctl.begin");
    int prime_result = ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
    sysfs_step("cap-prime.ioctl.returned");
    if (prime_result < 0 ||
        cap.value != (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) {
        printf("DRM_PRESENT PRIME query errno=%d value=%llu\n", errno,
               (unsigned long long)cap.value);
        close(fd);
        return 15;
    }
    cap.capability = DRM_CAP_DUMB_BUFFER;
    sysfs_step("cap-dumb.ioctl.begin");
    int dumb_result = ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
    sysfs_step("cap-dumb.ioctl.returned");
    printf("DRM_CAP dumb=%d value=%llu errno=%d\n", dumb_result,
           (unsigned long long)cap.value, errno);
    if (dumb_result < 0 || cap.value != 1) {
        close(fd);
        return 16;
    }

    struct pollfd pollfd = {.fd = fd, .events = POLLIN};
    sysfs_step("empty-poll.begin");
    int poll_result = poll(&pollfd, 1, 0);
    sysfs_step("empty-poll.returned");
    if (poll_result != 0 || pollfd.revents != 0) {
        printf("DRM_PRESENT empty poll returned revents=%x\n",
               (unsigned)pollfd.revents);
        close(fd);
        return 17;
    }
    char event[sizeof(struct drm_event)];
    errno = 0;
    sysfs_step("empty-read.begin");
    ssize_t read_result = read(fd, event, sizeof(event));
    sysfs_step("empty-read.returned");
    int read_error = errno;
    printf("DRM_EMPTY_READ result=%ld errno=%d\n",
           (long)read_result, read_error);
    if (read_result != -1 || read_error != EAGAIN) {
        close(fd);
        return 18;
    }
    int lseek_result = check_noop_lseek(fd);
    if (lseek_result) {
        close(fd);
        return 21;
    }
    int interruptible_result = check_interruptible_read(fd);
    if (interruptible_result) {
        close(fd);
        return 22;
    }
    int positional_result = check_positional_read(fd, "pread64");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv2");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv2-current");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv2-hipri");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv2-current-hipri");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv-zero-negative");
    if (!positional_result) positional_result = check_positional_read(fd, "preadv2-zero-negative");
    if (positional_result) {
        close(fd);
        return 23;
    }
    sysfs_step("open-lifetime.begin");
    int lifetime_result = check_open_file_lifetime(fd);
    sysfs_step("open-lifetime.returned");
    if (lifetime_result) {
        printf("DRM_PRESENT open-file lifetime check failed step=%d errno=%d\n",
               lifetime_result, errno);
        return 20;
    }
    return 0;
}

static int bo_failure(const char *stage, int status)
{
    printf("DRM_BO_FAIL stage=%s status=%d errno=%d\n", stage, status, errno);
    return status;
}

static int secondary_primary_bo_child(void)
{
    if (geteuid() == 0) {
        if (setgid(44) < 0 || setuid(1000) < 0) return 81;
    } else if (geteuid() != 1000) {
        return 82;
    }

    int first = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (first < 0) return 83;
    int second = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (second < 0) {
        close(first);
        return 84;
    }

    struct drm_mode_create_dumb create = {.width = 32, .height = 32, .bpp = 32};
    if (ioctl(second, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 || !create.handle) {
        close(second);
        close(first);
        return 85;
    }
    struct drm_mode_map_dumb map = {.handle = create.handle};
    if (ioctl(second, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0 || !map.offset) {
        close(second);
        close(first);
        return 86;
    }
    if (ioctl(second, DRM_IOCTL_GEM_CLOSE, &(struct drm_gem_close){.handle = create.handle}) < 0) {
        close(second);
        close(first);
        return 87;
    }
    if (close(second) < 0 || close(first) < 0) return 88;
    return 0;
}

static int check_secondary_primary_bo(void)
{
    pid_t child = fork();
    if (child < 0) return bo_failure("secondary-primary-fork", 80);
    if (child == 0) _exit(secondary_primary_bo_child());
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        printf("DRM_BO_FAIL stage=secondary-primary status=%d errno=%d\n",
               status, errno);
        return 89;
    }
    puts("DRM_BO_SECONDARY_PRIMARY PASS uid=1000 secondary=unauthenticated");
    return 0;
}

struct drm_smp_protect_state {
    volatile uint32_t ready;
    volatile uint32_t release;
    volatile unsigned char *address;
    uint32_t cpu;
};

static int drm_smp_protect_child(void *opaque)
{
    struct drm_smp_protect_state *state = opaque;
    uint64_t mask = UINT64_C(1) << state->cpu;
    if (syscall(SYS_sched_setaffinity, 0, (long)sizeof(mask), &mask) != 0)
        _exit(91);
    bool on_target_cpu = false;
    for (unsigned poll = 0; poll < 10000; ++poll) {
        unsigned cpu = UINT32_MAX;
        if (syscall(SYS_getcpu, &cpu, NULL, NULL) != 0) _exit(92);
        if (cpu == state->cpu) {
            on_target_cpu = true;
            break;
        }
        syscall(SYS_sched_yield);
    }
    if (!on_target_cpu) _exit(93);

    unsigned char observed = state->address[0];
    /* Exercise writable translation and dirty permission before mprotect. */
    state->address[0] = observed;
    __atomic_store_n(&state->ready, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&state->release, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" : : : "memory");

    /* No syscall or CR3 transition occurs between priming this writable TLB
     * entry, the parent's mprotect, and this store. A stale entry succeeds. */
    state->address[0] = (unsigned char)(observed + 1u);
    _exit(0);
}

static int check_remote_drm_mprotect(int fd)
{
    uint64_t original_mask = 0;
    long mask_size = syscall(SYS_sched_getaffinity, 0,
                             (long)sizeof(original_mask), &original_mask);
    if (mask_size != (long)sizeof(original_mask))
        return bo_failure("mprotect-smp-getaffinity", 94);
    uint32_t first_cpu = UINT32_MAX, second_cpu = UINT32_MAX;
    for (uint32_t cpu = 0; cpu < 64; ++cpu) {
        if (!(original_mask & (UINT64_C(1) << cpu))) continue;
        if (first_cpu == UINT32_MAX) first_cpu = cpu;
        else { second_cpu = cpu; break; }
    }
    if (second_cpu == UINT32_MAX) {
        puts("SKIP DRM_BO_MPROTECT_SMP: one CPU; remaining BO/mapping checks still run");
        return 0;
    }

    uint64_t first_mask = UINT64_C(1) << first_cpu;
    if (syscall(SYS_sched_setaffinity, 0, (long)sizeof(first_mask),
                &first_mask) != 0)
        return bo_failure("mprotect-smp-pin-parent", 96);

    struct drm_mode_create_dumb create = {
        .width = 1, .height = 1, .bpp = 32,
    };
    struct drm_mode_map_dumb map = {0};
    void *mapping = MAP_FAILED;
    void *child_stack = NULL;
    pid_t child = -1;
    int result = 0, child_status = 0;
    struct drm_smp_protect_state state = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0 ||
        !create.handle || create.size != 4096) {
        result = bo_failure("mprotect-smp-create", 97);
        goto out;
    }
    map.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        result = bo_failure("mprotect-smp-map-token", 98);
        goto out;
    }
    mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)map.offset);
    if (mapping == MAP_FAILED) {
        result = bo_failure("mprotect-smp-mmap", 99);
        goto out;
    }
    ((volatile unsigned char *)mapping)[0] = 0x39;
    child_stack = malloc(64 * 1024);
    if (!child_stack) {
        result = bo_failure("mprotect-smp-stack", 100);
        goto out;
    }
    state = (struct drm_smp_protect_state){
        .address = mapping,
        .cpu = second_cpu,
    };
    child = clone(drm_smp_protect_child,
                  (unsigned char *)child_stack + 64 * 1024,
                  CLONE_VM | SIGCHLD, &state);
    if (child < 0) {
        result = bo_failure("mprotect-smp-clone", 101);
        goto out;
    }

    bool child_ready = false;
    for (uint32_t poll = 0; poll < 50000000; ++poll) {
        if (__atomic_load_n(&state.ready, __ATOMIC_ACQUIRE)) {
            child_ready = true;
            break;
        }
        __asm__ volatile("pause" : : : "memory");
    }
    if (!child_ready) {
        __atomic_store_n(&state.release, 1u, __ATOMIC_RELEASE);
        kill(child, SIGKILL);
        waitpid(child, &child_status, 0);
        child = -1;
        result = bo_failure("mprotect-smp-child-ready-timeout", 102);
        goto out;
    }
    if (mprotect(mapping, 4096, PROT_READ) < 0) {
        result = bo_failure("mprotect-smp-revoke", 103);
        __atomic_store_n(&state.release, 1u, __ATOMIC_RELEASE);
        goto out;
    }
    __atomic_store_n(&state.release, 1u, __ATOMIC_RELEASE);
    pid_t waited;
    do {
        waited = waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        child = -1;
        result = bo_failure("mprotect-smp-wait", 104);
        goto out;
    }
    child = -1;
    if (!WIFSIGNALED(child_status) || WTERMSIG(child_status) != SIGSEGV) {
        printf("DRM_BO_FAIL stage=mprotect-smp-stale-write status=%d errno=%d\n",
               child_status, errno);
        result = 105;
        goto out;
    }
    puts("DRM_BO_MPROTECT_SMP PASS shared_mm=1 remote_cpu=1 stale_write=SIGSEGV");

out:
    if (child > 0) {
        __atomic_store_n(&state.release, 1u, __ATOMIC_RELEASE);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    if (mapping != MAP_FAILED) {
        (void)mprotect(mapping, 4096, PROT_READ | PROT_WRITE);
        if (munmap(mapping, 4096) < 0 && !result)
            result = bo_failure("mprotect-smp-munmap", 106);
    }
    if (create.handle &&
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB,
              &(struct drm_mode_destroy_dumb){.handle = create.handle}) < 0 &&
        !result)
        result = bo_failure("mprotect-smp-destroy", 107);
    free(child_stack);
    if (syscall(SYS_sched_setaffinity, 0, (long)sizeof(original_mask),
                &original_mask) != 0 && !result)
        result = bo_failure("mprotect-smp-restore-affinity", 108);
    return result;
}

static int expect_bo(void)
{
    struct drm_get_cap cap = {.capability = DRM_CAP_DUMB_BUFFER};
    struct drm_mode_create_dumb create = {0};
    struct drm_mode_map_dumb map = {0};
    struct drm_gem_close close_gem = {0};
    struct drm_set_version set_version = {
        .drm_di_major = 1, .drm_di_minor = 4,
        .drm_dd_major = -1, .drm_dd_minor = -1,
    };
    int fd = -1, foreign = -1, candidate = -1, reopened = -1;
    void *mapping = MAP_FAILED;
    size_t map_size;
    pid_t child;
    int child_status = 0;

    int secondary = check_secondary_primary_bo();
    if (secondary) return secondary;

    sysfs_step("bo.open.begin");
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    sysfs_step("bo.open.returned");
    if (fd < 0) return bo_failure("open", 30);
    foreign = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (foreign < 0) return bo_failure("foreign-open", 31);

    sysfs_step("bo.cap-dumb.begin");
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0 || cap.value != 1)
        return bo_failure("cap-dumb", 32);
    cap.capability = DRM_CAP_DUMB_PREFERRED_DEPTH;
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0 || cap.value != 32)
        return bo_failure("cap-depth", 33);
    cap.capability = DRM_CAP_DUMB_PREFER_SHADOW;
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0 || cap.value != 0)
        return bo_failure("cap-shadow", 34);
    sysfs_step("bo.cap-dumb.returned");

    int smp_protect = check_remote_drm_mprotect(fd);
    if (smp_protect) return smp_protect;

    struct drm_mode_create_dumb invalid = {
        .height = 0, .width = 32, .bpp = 32,
        .handle = UINT32_MAX, .pitch = UINT32_MAX, .size = UINT64_MAX,
    };
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &invalid) != -1 ||
        errno != EINVAL || invalid.handle || invalid.pitch || invalid.size)
        return bo_failure("invalid-create", 35);

    /* A read-only output buffer makes Linux copy-to-user fail after the
     * production handler has attempted an allocation. The handle must roll
     * back when that copy fails. */
    void *output_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (output_page == MAP_FAILED) return bo_failure("copyout-map", 36);
    struct drm_mode_create_dumb *faulting = output_page;
    *faulting = (struct drm_mode_create_dumb){
        .height = 4, .width = 64, .bpp = 32,
    };
    if (mprotect(output_page, 4096, PROT_READ) < 0)
        return bo_failure("copyout-protect", 37);
    errno = 0;
    int copyout_result = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, faulting);
    int copyout_errno = errno;
    if (mprotect(output_page, 4096, PROT_READ | PROT_WRITE) < 0)
        return bo_failure("copyout-unprotect", 38);
    if (munmap(output_page, 4096) < 0) return bo_failure("copyout-unmap", 39);
    if (copyout_result != -1 || copyout_errno != EFAULT)
        return bo_failure("copyout-rollback", 40);

    create.width = 1024;
    create.height = 4;
    create.bpp = 32;
    create.flags = 0;
    sysfs_step("bo.create-dumb.begin");
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
        return bo_failure("create-dumb", 41);
    sysfs_step("bo.create-dumb.returned");
    if (!create.handle || create.pitch != create.width * 4u ||
        create.size < (uint64_t)create.pitch * create.height ||
        (create.size & 4095u))
        return bo_failure("create-layout", 42);
    map.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0 ||
        !map.offset || (map.offset & 4095u))
        return bo_failure("map-dumb", 43);
    int write_only = open("/dev/dri/card0", O_WRONLY | O_CLOEXEC);
    if (write_only < 0) return bo_failure("open-write-only", 75);
    struct drm_mode_create_dumb write_only_create = {
        .width = 1, .height = 1, .bpp = 32,
    };
    if (ioctl(write_only, DRM_IOCTL_MODE_CREATE_DUMB, &write_only_create) < 0)
        return bo_failure("create-write-only", 76);
    struct drm_mode_map_dumb write_only_map = {
        .handle = write_only_create.handle,
    };
    if (ioctl(write_only, DRM_IOCTL_MODE_MAP_DUMB, &write_only_map) < 0)
        return bo_failure("map-write-only", 77);
    errno = 0;
    void *no_read_mapping = mmap(NULL, 4096, PROT_NONE, MAP_SHARED,
                                 write_only, (off_t)write_only_map.offset);
    if (no_read_mapping != MAP_FAILED) {
        munmap(no_read_mapping, 4096);
        return bo_failure("mmap-write-only-accepted", 78);
    }
    if (errno != EACCES ||
        ioctl(write_only, DRM_IOCTL_MODE_DESTROY_DUMB,
              &(struct drm_mode_destroy_dumb){
                  .handle = write_only_create.handle,
              }) < 0 || close(write_only) < 0)
        return bo_failure("mmap-write-only-errno", 79);
    errno = 0;
    void *foreign_map = mmap(NULL, (size_t)create.size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, foreign, (off_t)map.offset);
    if (foreign_map != MAP_FAILED) {
        munmap(foreign_map, (size_t)create.size);
        return bo_failure("foreign-map-authorized", 44);
    }
    errno = 0;
    void *private_map = mmap(NULL, (size_t)create.size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE, fd, (off_t)map.offset);
    if (private_map != MAP_FAILED) {
        munmap(private_map, (size_t)create.size);
        return bo_failure("private-cow-map-accepted", 45);
    }
    errno = 0;
    void *unaligned_map = mmap(NULL, (size_t)create.size, PROT_READ,
                               MAP_SHARED, fd, (off_t)(map.offset + 1));
    if (unaligned_map != MAP_FAILED) {
        munmap(unaligned_map, (size_t)create.size);
        return bo_failure("unaligned-map-offset", 46);
    }
    map_size = (size_t)create.size;
    mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)map.offset);
    if (mapping == MAP_FAILED) return bo_failure("mmap-dumb", 47);
    for (size_t i = 0; i < map_size; ++i)
        if (((volatile unsigned char *)mapping)[i] != 0)
            return bo_failure("zero-backing", 48);
    ((volatile unsigned char *)mapping)[0] = 0x31;
    ((volatile unsigned char *)mapping)[2 * 4096] = 0x42;
    void *move_target = mmap(NULL, map_size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (move_target == MAP_FAILED || munmap(move_target, map_size) < 0)
        return bo_failure("mremap-target", 49);
    errno = 0;
    void *bad_move = mremap(mapping, map_size, map_size,
                            MREMAP_MAYMOVE | MREMAP_FIXED,
                            (unsigned char *)move_target + 1);
    if (bad_move != MAP_FAILED || errno != EINVAL ||
        ((volatile unsigned char *)mapping)[0] != 0x31)
        return bo_failure("mremap-invalid-fixed-preserves-source", 50);
    void *moved = mremap(mapping, map_size, map_size,
                         MREMAP_MAYMOVE | MREMAP_FIXED, move_target);
    if (moved == MAP_FAILED || moved != move_target ||
        ((volatile unsigned char *)moved)[0] != 0x31 ||
        ((volatile unsigned char *)moved)[2 * 4096] != 0x42)
        return bo_failure("mremap-fixed-move", 51);
    mapping = moved;
    if (mprotect((unsigned char *)mapping + 4096, 4096, PROT_READ) < 0)
        return bo_failure("mprotect-split", 52);
    if (munmap((unsigned char *)mapping + 4096, 4096) < 0)
        return bo_failure("munmap-middle", 53);
    errno = 0;
    void *expanded = mremap(mapping, 4096, 8192, MREMAP_MAYMOVE);
    if (expanded != MAP_FAILED) {
        munmap(expanded, 8192);
        return bo_failure("mremap-expanded-device", 54);
    }
    if (errno != EFAULT) return bo_failure("mremap-error", 55);

    sysfs_step("bo.fork.begin");
    child = fork();
    if (child < 0) return bo_failure("fork", 56);
    if (child == 0) {
        if (((volatile unsigned char *)mapping)[0] != 0x31 ||
            ((volatile unsigned char *)mapping)[2 * 4096] != 0x42)
            _exit(57);
        ((volatile unsigned char *)mapping)[0] = 0x53;
        ((volatile unsigned char *)mapping)[2 * 4096] = 0x64;
        if (close(fd) < 0 || munmap(mapping, 4096) < 0 ||
            munmap((unsigned char *)mapping + 8192, 8192) < 0)
            _exit(58);
        _exit(0);
    }
    if (waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
        return bo_failure("fork-map-lifetime", 59);
    sysfs_step("bo.fork.returned");
    if (((volatile unsigned char *)mapping)[0] != 0x53 ||
        ((volatile unsigned char *)mapping)[2 * 4096] != 0x64)
        return bo_failure("fork-shared-backing", 60);

    close_gem.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_gem) < 0)
        return bo_failure("gem-close", 61);
    if (((volatile unsigned char *)mapping)[0] != 0x53)
        return bo_failure("map-after-gem-close", 62);
    errno = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != -1 || errno != ENOENT)
        return bo_failure("map-handle-after-close", 63);
    if (close(fd) < 0) return bo_failure("close-last-fd", 64);
    fd = -1;
    candidate = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (candidate < 0) return bo_failure("master-lifetime-open", 65);
    errno = 0;
    if (ioctl(candidate, DRM_IOCTL_SET_VERSION, &set_version) != -1 ||
        errno != EACCES)
        return bo_failure("mapped-master-released-early", 66);
    if (munmap(mapping, 4096) < 0 ||
        munmap((unsigned char *)mapping + 8192, 8192) < 0)
        return bo_failure("final-munmap", 67);
    mapping = MAP_FAILED;
    close(candidate);
    candidate = -1;
    reopened = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (reopened < 0 || ioctl(reopened, DRM_IOCTL_SET_VERSION, &set_version) < 0)
        return bo_failure("master-after-munmap", 68);
    close(foreign);
    foreign = -1;

    if (close(reopened) < 0)
        return bo_failure("close-reopened", 69);
    printf("DRM_BO PASS handle=%u mapped_bytes=%zu\n", create.handle, map_size);
    return 0;
}

static int kms_failure(const char *stage, int status)
{
    printf("DRM_KMS_FAIL stage=%s status=%d errno=%d\n", stage, status, errno);
    return status;
}

static int expect_kms(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return kms_failure("open", 110);

    int result = 0;
    struct drm_mode_card_res resources = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0 ||
        resources.count_crtcs != 1 || resources.count_connectors != 1 ||
        resources.count_encoders != 1) {
        result = kms_failure("resource-counts", 111);
        goto out;
    }
    uint32_t crtc_id = 0, connector_id = 0, encoder_id = 0;
    resources.crtc_id_ptr = (uintptr_t)&crtc_id;
    resources.connector_id_ptr = (uintptr_t)&connector_id;
    resources.encoder_id_ptr = (uintptr_t)&encoder_id;
    resources.count_fbs = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0 ||
        !crtc_id || !connector_id || !encoder_id) {
        result = kms_failure("resource-ids", 112);
        goto out;
    }

    struct drm_mode_get_connector connector = {.connector_id = connector_id};
    struct drm_mode_modeinfo modes[32] = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0 ||
        !connector.count_modes ||
        connector.count_modes > sizeof(modes) / sizeof(*modes) ||
        connector.count_encoders != 1 || connector.encoder_id != encoder_id ||
        connector.connector_type != DRM_MODE_CONNECTOR_VIRTUAL ||
        connector.connection != 1) {
        result = kms_failure("connector-counts", 113);
        goto out;
    }
    uint32_t connector_encoder = 0;
    connector.modes_ptr = (uintptr_t)modes;
    connector.encoders_ptr = (uintptr_t)&connector_encoder;
    connector.count_props = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0 ||
        !modes[0].hdisplay || !modes[0].vdisplay ||
        !(modes[0].type & DRM_MODE_TYPE_PREFERRED) || connector_encoder != encoder_id) {
        result = kms_failure("connector-data", 114);
        goto out;
    }

    struct drm_mode_get_encoder encoder = {.encoder_id = encoder_id};
    struct drm_mode_crtc crtc = {.crtc_id = crtc_id};
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) < 0 ||
        !(encoder.possible_crtcs & 1) || ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0) {
        result = kms_failure("encoder-crtc", 115);
        goto out;
    }

    char status[32];
    if (read_attribute("connector-status", "/sys/class/drm/card0-Virtual-1/status",
                       status, sizeof(status)) < 0 || strcmp(status, "connected\n")) {
        result = kms_failure("sysfs-status", 116);
        goto out;
    }
    printf("DRM_KMS PASS virtual=1 preferred=%ux%u\n", modes[0].hdisplay, modes[0].vdisplay);

out:
    if (close(fd) < 0 && !result) result = kms_failure("close", 117);
    return result;
}

int main(int argc, char **argv)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT")) {
        fputs("Guest-only DRM probe: refusing host mutations\n",stderr);return 77;
    }
    if (setvbuf(stdout, NULL, _IONBF, 0) != 0)
        return 70;
    if (argc == 2 && !strcmp(argv[1], "expect-absent"))
        return expect_absent();
    if (argc == 2 && !strcmp(argv[1], "expect-present"))
        return expect_present();
    if (argc == 2 && !strcmp(argv[1], "expect-bo"))
        return expect_bo();
    if (argc == 2 && !strcmp(argv[1], "expect-kms"))
        return expect_kms();
    if (argc == 2 && !strncmp(argv[1], "positional-", 11))
        return run_positional_probe(argv[1] + 11);
    fprintf(stderr, "usage: %s [expect-absent|expect-present|expect-bo|expect-kms|positional-pread64|positional-preadv|positional-preadv2|positional-preadv2-current|positional-preadv2-hipri|positional-preadv2-current-hipri|positional-preadv-zero-negative|positional-preadv2-zero-negative|positional-lseek|positional-interruptible]\n", argv[0]);
    return 64;
}

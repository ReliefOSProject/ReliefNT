/* Exercise the real syscall wrapper with failing storage/descriptor boundaries. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/syscall/event/timerfd.c"

static struct task caller;
static struct task_file slot;
static int attach_error, truncate_error;
static unsigned closes;
static int allocated_fd = 3;
static char captured_name[250];

struct task *sched_current_task(void) { return &caller; }
int alloc_task_fd(struct task *task, const struct storage_node *node,
                  uint32_t flags, const char *path)
{
    (void)path;
    assert(task == &caller && !slot.used);
    slot = (struct task_file){.used = 1, .node = *node, .flags = flags};
    if (allocated_fd < 3) caller.closed_stdio_mask &= ~(1u << allocated_fd);
    return allocated_fd;
}
struct task_file *task_file_for_fd(struct task *task, int fd)
{ assert(task == &caller && fd == allocated_fd); return &slot; }
void clear_task_file(struct task_file *file)
{ assert(file == &slot && file->used); ++closes; *file = (struct task_file){0}; }
void task_discard_file_fd(struct task *task, int fd)
{
    assert(task == &caller && fd == allocated_fd);
    clear_task_file(&slot);
    if (fd < 3) caller.closed_stdio_mask |= 1u << fd;
}
int copy_user_string_fixed(char *dst, uint32_t cap, uint64_t pointer, uint32_t *length)
{
    if (!pointer) return -RELIEFOS_EFAULT;
    size_t size = strlen((const char *)(uintptr_t)pointer);
    assert(size < cap);
    memcpy(dst, (const void *)(uintptr_t)pointer, size + 1);
    if (length) *length = (uint32_t)size;
    return 0;
}
bool user_range_ok(uint64_t pointer,uint64_t length)
{ return pointer>=4096 && length<=UINT64_MAX-pointer; }
size_t user_strlen(const char *text,size_t maximum)
{ size_t length=0;while(length<maximum && text[length])++length;return length; }
int task_shm_attach(struct task_file *file, uint32_t flags)
{ assert(file == &slot); (void)flags; return attach_error; }
int task_shm_truncate(struct task_file *file, uint64_t size)
{ assert(file == &slot && !size); return truncate_error; }
int task_shm_set_name(struct task_file *file,const char *name,size_t length)
{ assert(file==&slot && length<sizeof(captured_name));memcpy(captured_name,name,length);captured_name[length]=0;return 0; }

int main(void)
{
    const uint64_t name = (uintptr_t)"error-probe";
    const int errors[] = {-RELIEFOS_ENFILE, -RELIEFOS_ENOMEM, -RELIEFOS_ENOSPC};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        attach_error = errors[i];
        int64_t result = syscall_memfd_create(name, LINUX_MFD_ALLOW_SEALING);
        if (result != errors[i])
            fprintf(stderr, "memfd resource errno collapsed: got %lld, expected %d\n",
                    (long long)result, errors[i]);
        assert(result == errors[i] && !slot.used && closes == i + 1);
    }
    attach_error = 0;
    truncate_error = -RELIEFOS_ENOSPC;
    assert(syscall_memfd_create(name, 0) == truncate_error && !slot.used && closes == 4);
    truncate_error = 0;
    assert(syscall_memfd_create(name, LINUX_MFD_CLOEXEC) == 3);
    assert(slot.used && slot.fd_flags == RELIEFOS_FD_CLOEXEC);
    assert(!strcmp(captured_name,"error-probe"));
    clear_task_file(&slot);
    assert(syscall_memfd_create((uintptr_t)"",0)==3);
    assert(!captured_name[0]);clear_task_file(&slot);
    char maximum[251];memset(maximum,'a',sizeof(maximum));maximum[249]=0;
    assert(syscall_memfd_create((uintptr_t)maximum,0)==3);
    assert(strlen(captured_name)==249);clear_task_file(&slot);
    maximum[249]='a';maximum[250]=0;
    assert(syscall_memfd_create((uintptr_t)maximum,0)==-RELIEFOS_EINVAL && !slot.used);
    assert(syscall_memfd_create(0,0)==-RELIEFOS_EFAULT && !slot.used);
    for (allocated_fd = 0; allocated_fd < 3; ++allocated_fd) {
        caller.closed_stdio_mask = 7;
        attach_error = -RELIEFOS_ENFILE;
        assert(syscall_memfd_create(name, 0) == attach_error);
        assert(!slot.used && caller.closed_stdio_mask == 7);
        attach_error = 0;
        truncate_error = -RELIEFOS_ENOSPC;
        assert(syscall_memfd_create(name, 0) == truncate_error);
        assert(!slot.used && caller.closed_stdio_mask == 7);
        truncate_error = 0;
    }
    puts("PASS memfd syscall: resource errno, failed-descriptor rollback and CLOEXEC");
    return 0;
}

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <reliefnt/object.h>
#include <reliefnt/console.h>

static unsigned locked;
static int fail_alloc;
static struct kernel_object_table table;
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state = 0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; assert(!locked++); }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; assert(locked-- == 1); }
void *kernel_malloc(size_t bytes) { assert(!locked); return fail_alloc ? NULL : malloc(bytes); }
void kernel_free(void *value) { assert(!locked); free(value); }
void console_log(enum console_log_level level, const char *format, ...) { (void)level; (void)format; }

int main(void)
{
    static int objects[700];
    static uint32_t handles[700];
    kernel_object_table_init(&table);
    for (unsigned i = 0; i < 700; ++i) {
        handles[i] = kernel_object_insert(&table, &objects[i], KERNEL_OBJECT_PIPE);
        if (!handles[i]) fprintf(stderr, "object allocation stopped at %u\n", i);
        assert(handles[i]);
        assert(kernel_object_lookup(&table, handles[i], KERNEL_OBJECT_PIPE) == &objects[i]);
        assert(!kernel_object_lookup(&table, handles[i], KERNEL_OBJECT_SOCKET));
    }
    uint32_t stale = handles[12];
    void *removed = NULL;
    assert(!kernel_object_remove(&table, stale, KERNEL_OBJECT_PIPE, &removed));
    assert(removed == &objects[12]);
    assert(!kernel_object_lookup(&table, stale, KERNEL_OBJECT_NONE));
    uint32_t fresh = kernel_object_insert(&table, &objects[12], KERNEL_OBJECT_PIPE);
    assert(fresh && fresh != stale);
    assert(!kernel_object_lookup(&table, stale, KERNEL_OBJECT_PIPE));
    handles[12] = fresh;
    for (unsigned i = 0; i < 700; ++i)
        assert(!kernel_object_remove(&table, handles[i], KERNEL_OBJECT_PIPE, NULL));
    /* Fill the allocated registry, then fail a real growth allocation. */
    uint32_t capacity = table.capacity;
    uint32_t *full = calloc(capacity, sizeof(*full));
    assert(full);
    fail_alloc = 1;
    for (uint32_t i = 0; i < capacity; ++i) {
        full[i] = kernel_object_insert(&table, &objects[i % 700], KERNEL_OBJECT_DEVICE);
        assert(full[i]);
    }
    assert(!kernel_object_insert(&table, &objects[0], KERNEL_OBJECT_DEVICE));
    assert(table.capacity == capacity);
    for (uint32_t i = 0; i < capacity; ++i)
        assert(kernel_object_lookup(&table, full[i], KERNEL_OBJECT_DEVICE) == &objects[i % 700]);
    fail_alloc = 0;
    uint32_t extra = kernel_object_insert(&table, &objects[0], KERNEL_OBJECT_DEVICE);
    assert(extra);
    for (uint32_t i = 0; i < capacity; ++i)
        assert(!kernel_object_remove(&table, full[i], KERNEL_OBJECT_DEVICE, NULL));
    assert(!kernel_object_remove(&table, extra, KERNEL_OBJECT_DEVICE, NULL));
    free(full);
    puts("PASS object handles: growth, type/generation safety, allocation rollback outside spinlock");
}

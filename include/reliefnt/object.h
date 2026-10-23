/*
 * ReliefOS kernel object table.
 * Converts internal object pointers into generation-checked process handles.
 */
#ifndef RELIEFNT_OBJECT_H
#define RELIEFNT_OBJECT_H

#include <reliefnt/lock.h>
#include <reliefnt/types.h>

#define KERNEL_OBJECT_MAX 256u
/* Internal handles reserve 16 bits for a nonzero, one-based slot. */
#define KERNEL_OBJECT_HANDLE_SLOTS 65535u
#define KERNEL_HANDLE_INVALID 0u

enum kernel_object_type {
    KERNEL_OBJECT_NONE = 0,
    KERNEL_OBJECT_FILE = 1,
    KERNEL_OBJECT_PIPE = 2,
    KERNEL_OBJECT_SOCKET = 3,
    KERNEL_OBJECT_DEVICE = 4,
    KERNEL_OBJECT_VM = 5,
};

struct kernel_object_slot {
    void *object;
    uint16_t generation;
    uint16_t type;
};

struct kernel_object_table {
    struct kernel_spinlock lock;
    void *objects[KERNEL_OBJECT_MAX];
    uint16_t generations[KERNEL_OBJECT_MAX];
    uint16_t types[KERNEL_OBJECT_MAX];
    struct kernel_object_slot *extended;
    uint32_t capacity;
};

void kernel_object_table_init(struct kernel_object_table *table);
void kernel_objects_init(void);
struct kernel_object_table *kernel_objects(void);
uint32_t kernel_object_insert(struct kernel_object_table *table, void *object,
                              enum kernel_object_type type);
void *kernel_object_lookup(struct kernel_object_table *table, uint32_t handle,
                           enum kernel_object_type type);
int kernel_object_remove(struct kernel_object_table *table, uint32_t handle,
                         enum kernel_object_type type, void **object);

#endif

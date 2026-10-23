/* Generation-checked kernel handles. Objects never move when the table grows. */
#include <reliefnt/object.h>
#include <reliefnt/console.h>
#include <reliefnt/heap.h>

static struct kernel_object_table global_objects;

static uint32_t make_handle(uint32_t index, uint16_t generation)
{
    return ((uint32_t)generation << 16) | (index + 1u);
}

static uint32_t handle_index(uint32_t handle)
{
    uint32_t value = handle & 0xffffu;
    return value ? value - 1u : KERNEL_OBJECT_HANDLE_SLOTS;
}

static struct kernel_object_slot read_slot(const struct kernel_object_table *table,
                                            uint32_t index)
{
    return table->extended ? table->extended[index] : (struct kernel_object_slot){
        .object = table->objects[index], .generation = table->generations[index],
        .type = table->types[index]};
}

static void write_slot(struct kernel_object_table *table, uint32_t index,
                        struct kernel_object_slot slot)
{
    if (table->extended) table->extended[index] = slot;
    else {
        table->objects[index] = slot.object;
        table->generations[index] = slot.generation;
        table->types[index] = slot.type;
    }
}

void kernel_object_table_init(struct kernel_object_table *table)
{
    if (!table) return;
    /* Initialization is only valid for an unpublished, uninitialized table. */
    *table = (struct kernel_object_table){.capacity = KERNEL_OBJECT_MAX};
    kernel_spin_init(&table->lock);
    for (uint32_t i = 0; i < KERNEL_OBJECT_MAX; ++i)
        table->generations[i] = 1;
}

void kernel_objects_init(void)
{
    kernel_object_table_init(&global_objects);
    console_info("[objects] initial_slots=%u handle_slots=%u generation_bits=16\n",
                 KERNEL_OBJECT_MAX, KERNEL_OBJECT_HANDLE_SLOTS);
}

struct kernel_object_table *kernel_objects(void)
{
    return &global_objects;
}

uint32_t kernel_object_insert(struct kernel_object_table *table, void *object,
                              enum kernel_object_type type)
{
    if (!table || !object || type == KERNEL_OBJECT_NONE) return KERNEL_HANDLE_INVALID;
    for (;;) {
        uint64_t flags;
        kernel_spin_lock_irqsave(&table->lock, &flags);
        uint32_t capacity = table->capacity;
        for (uint32_t i = 0; i < capacity; ++i) {
            struct kernel_object_slot slot = read_slot(table, i);
            if (slot.object) continue;
            if (!++slot.generation) ++slot.generation;
            slot.object = object;
            slot.type = (uint16_t)type;
            write_slot(table, i, slot);
            uint32_t handle = make_handle(i, slot.generation);
            kernel_spin_unlock_irqrestore(&table->lock, flags);
            return handle;
        }
        kernel_spin_unlock_irqrestore(&table->lock, flags);
        if (capacity == KERNEL_OBJECT_HANDLE_SLOTS) return KERNEL_HANDLE_INVALID;
        uint32_t grown = capacity * 2u;
        if (grown > KERNEL_OBJECT_HANDLE_SLOTS) grown = KERNEL_OBJECT_HANDLE_SLOTS;
        struct kernel_object_slot *replacement = kernel_malloc((size_t)grown * sizeof(*replacement));
        if (!replacement) return KERNEL_HANDLE_INVALID;
        for (uint32_t i = 0; i < grown; ++i)
            replacement[i] = (struct kernel_object_slot){.generation = 1};
        kernel_spin_lock_irqsave(&table->lock, &flags);
        struct kernel_object_slot *old = NULL;
        if (table->capacity == capacity) {
            for (uint32_t i = 0; i < capacity; ++i) replacement[i] = read_slot(table, i);
            old = table->extended;
            table->extended = replacement;
            table->capacity = grown;
            replacement = NULL;
        }
        kernel_spin_unlock_irqrestore(&table->lock, flags);
        kernel_free(old);
        kernel_free(replacement);
    }
}

void *kernel_object_lookup(struct kernel_object_table *table, uint32_t handle,
                           enum kernel_object_type type)
{
    uint32_t index = handle_index(handle);
    if (!table || !handle) return NULL;
    uint64_t flags;
    void *object = NULL;
    kernel_spin_lock_irqsave(&table->lock, &flags);
    if (index < table->capacity) {
        struct kernel_object_slot slot = read_slot(table, index);
        if (slot.generation == (uint16_t)(handle >> 16) &&
            (type == KERNEL_OBJECT_NONE || slot.type == (uint16_t)type)) object = slot.object;
    }
    kernel_spin_unlock_irqrestore(&table->lock, flags);
    return object;
}

int kernel_object_remove(struct kernel_object_table *table, uint32_t handle,
                         enum kernel_object_type type, void **object)
{
    uint32_t index = handle_index(handle);
    if (object) *object = NULL;
    if (!table || !handle) return -1;
    uint64_t flags;
    kernel_spin_lock_irqsave(&table->lock, &flags);
    if (index < table->capacity) {
        struct kernel_object_slot slot = read_slot(table, index);
        if (slot.object && slot.generation == (uint16_t)(handle >> 16) &&
            (type == KERNEL_OBJECT_NONE || slot.type == (uint16_t)type)) {
            if (object) *object = slot.object;
            slot.object = NULL;
            slot.type = KERNEL_OBJECT_NONE;
            write_slot(table, index, slot);
            kernel_spin_unlock_irqrestore(&table->lock, flags);
            return 0;
        }
    }
    kernel_spin_unlock_irqrestore(&table->lock, flags);
    return -1;
}

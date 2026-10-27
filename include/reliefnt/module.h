/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MODULE_H
#define RELIEFNT_MODULE_H

#include <reliefnt/types.h>

#define MODULE_NAME_LEN 56u
#define RELIEFNT_MODULE_ABI_VERSION 1u
/* Private pre-init file-snapshot retry; syscall entry consumes this value.
 * It is outside Linux's errno range and must never reach userspace. */
#define MODULE_FILE_IO_RETRY (-4097)
#define __RN_JOIN_(left, right) left##right
#define __RN_JOIN(left, right) __RN_JOIN_(left, right)
#define __RN_STRING_(value) #value
#define __RN_STRING(value) __RN_STRING_(value)
#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME "builtin"
#endif
#ifndef KBUILD_MODFILE
#define KBUILD_MODFILE KBUILD_MODNAME
#endif
#ifdef MODULE
#define __RN_INFO_PREFIX ""
#define MODULE_FILE
#else
#define __RN_INFO_PREFIX KBUILD_MODNAME "."
#define MODULE_FILE MODULE_INFO(file,KBUILD_MODFILE);
#endif

/* NUL-delimited .modinfo records are consumed by modpost and userspace tools. */
#define MODULE_INFO(tag, value) \
    static const char __RN_JOIN(__module_info_, __COUNTER__)[] \
    __attribute__((used, section(".modinfo"), aligned(1))) = __RN_INFO_PREFIX #tag "=" value
#define MODULE_LICENSE(value) MODULE_FILE MODULE_INFO(license, value)
#define MODULE_DESCRIPTION(value) MODULE_INFO(description, value)
#define MODULE_AUTHOR(value) MODULE_INFO(author, value)
#define MODULE_VERSION(value) MODULE_INFO(version, value)
#define MODULE_ALIAS(value) MODULE_INFO(alias, value)
#define MODULE_SOFTDEP(value) MODULE_INFO(softdep, value)
#define MODULE_IMPORT_NS(value) MODULE_INFO(import_ns, __RN_STRING(value))

struct module_runtime;
struct task_file;
struct task;

enum module_state { MODULE_COMING, MODULE_LIVE, MODULE_GOING };
struct module_status {
    char name[MODULE_NAME_LEN];
    enum module_state state;
    uint64_t size, references;
};

struct module_arch {
    uintptr_t reserved;
};

/* This object is emitted by original modpost. Runtime state is kernel-owned. */
struct module {
    char name[MODULE_NAME_LEN];
    int (*init)(void);
    void (*exit)(void);
    struct module_arch arch;
    struct module_runtime *runtime;
};

struct modversion_info {
    unsigned long crc;
    char name[MODULE_NAME_LEN];
};

struct module_initcall {
    const char *name;
    int (*init)(void);
    void (*exit)(void);
};

/** @brief Run builtin subsystems then ordinary devices once, retaining the first failure.
 * @return First negative initializer result or zero; repeated calls are idempotent.
 * Boot CPU only before userspace/AP execution, no concurrent calls. The caller
 * supplies the execution context; no module-table lock spans an initializer.
 */
int module_run_builtin_initcalls(void);
/** @brief Register linked builtin filesystems before any root filesystem probe.
 * @return First negative initializer result or zero, retained on later calls.
 * Boot CPU only after heap/IRQ setup, before storage root construction and
 * userspace/AP execution. Only fs_initcall records run; ordinary device
 * initializers retain their later phase. No module lock spans callbacks.
 */
int module_run_fs_initcalls(void);
/** @brief Discover/register builtin block devices before any root-volume probe.
 * @return First initializer error or zero, retained on repeated calls.
 * Boot CPU after memory, IRQ and driver manager setup, with one execution
 * transaction. Only block records run; ordinary input/network/sound stays later.
 * No module/registry lock spans native init or its explicitly suspended I/O.
 */
int module_run_block_initcalls(void);

/* Linked subsystem initcalls run before ordinary device records; an external
 * subsystem remains a normal module with one init entry and explicit deps. */
#ifdef MODULE
extern struct module __this_module;
#define THIS_MODULE (&__this_module)
#define module_init(function) \
    extern __typeof__(function) init_module __attribute__((alias(#function)))
#define module_exit(function) \
    extern __typeof__(function) cleanup_module __attribute__((alias(#function)))
#define fs_initcall(function) MODULE_INFO(reliefnt_phase,"filesystem"); module_init(function)
#define block_initcall(function) MODULE_INFO(reliefnt_phase,"block"); module_init(function)
#define subsys_initcall(function) module_init(function)
#else
#define THIS_MODULE ((struct module *)0)
#define module_init(function) \
    static const struct module_initcall __RN_JOIN(__initcall_, function) \
    __attribute__((used, section(".reliefnt.initcall"), aligned(8))) = \
        { KBUILD_MODNAME, function, (void (*)(void))0 }
#define module_exit(function) \
    static void (*const __RN_JOIN(__exitcall_, function))(void) \
    __attribute__((used, section(".reliefnt.exitcall"), aligned(8))) = function
#define subsys_initcall(function) \
    static const struct module_initcall __RN_JOIN(__subsys_initcall_, function) \
    __attribute__((used, section(".reliefnt.subsys_initcall"), aligned(8))) = \
        { KBUILD_MODNAME, function, (void (*)(void))0 }
#define fs_initcall(function) \
    MODULE_INFO(reliefnt_phase,"filesystem"); \
    static const struct module_initcall __RN_JOIN(__fs_initcall_, function) \
    __attribute__((used, section(".reliefnt.fs_initcall"), aligned(8))) = \
        { KBUILD_MODNAME, function, (void (*)(void))0 }
#define block_initcall(function) \
    MODULE_INFO(reliefnt_phase,"block"); \
    static const struct module_initcall __RN_JOIN(__block_initcall_, function) \
    __attribute__((used, section(".reliefnt.block_initcall"), aligned(8))) = \
        { KBUILD_MODNAME, function, (void (*)(void))0 }
#endif

/**
 * @brief Load a stable kernel-owned ET_REL image in sleepable task context.
 * @param image Immutable image storage owned by the caller for this call.
 * @param size Image length in bytes.
 * @param params Kernel-owned NUL-terminated parameter string; NULL means empty.
 * @param flags Linux init_module flags (zero); unsupported values are rejected.
 * @return Zero on success or a negative Linux errno; failure retains no image reference.
 */
int module_load_image(const void *image, size_t size, const char *params, unsigned int flags);

/**
 * @brief Read and load a referenced file without retaining a global module lock over I/O.
 * @param file Caller-owned stable open file reference; not a userspace pointer.
 * @param params Kernel-owned NUL-terminated parameters; NULL means empty.
 * @param flags Linux finit_module flags; force/version bypass is unsupported.
 * @return Zero, negative Linux errno, or MODULE_FILE_IO_RETRY before init while
 * file I/O waits. The caller keeps the same OFD across those retries; the task
 * owns an inode/read-deny lease until snapshot completion or module_file_cancel.
 * Initializer errors are final. Task context, no global module lock spans file I/O.
 */
int module_load_file(struct task_file *file, const char *params, unsigned int flags);

/** @brief Check file/flag eligibility without allocation, usercopy or I/O.
 * @param file Borrowed OFD held stable by execution ownership or an existing pin.
 * @param flags Linux finit_module flags with explicit unsupported-mode errors.
 * @return Zero or negative errno. Task context; ownership remains with caller.
 */
int module_file_validate(const struct task_file *file,unsigned int flags);
/** @brief Cancel an unfinished module-file snapshot and release its write denial.
 * @param task Task owning the snapshot inode pin; NULL is accepted.
 * Task context under execution ownership. Idempotent on completion, signal,
 * syscall replacement and exit; releases its inode pin after allowing writes.
 */
#ifdef CONFIG_MODULES
void module_file_cancel(struct task *task);
#else
static inline void module_file_cancel(struct task *task){(void)task;}
#endif


/**
 * @brief Close admission, drain callbacks and unload an unused module from task context.
 * @param name Kernel-owned NUL-terminated module name.
 * @param flags Linux delete_module flags, ignored as in Linux 6.12 with forced
 * unloading disabled. O_EXCL is accepted; O_TRUNC never bypasses live references.
 * @return Zero, -EWOULDBLOCK for external users, -EBUSY for initialization/drain, or another errno.
 */
int module_unload(const char *name, unsigned int flags);

/**
 * @brief Acquire an external reference only while the owner is LIVE; IRQ-safe.
 * @param owner Borrowed owner; NULL denotes permanent built-in code.
 * @return True if acquired or built-in; false when admission is closed.
 */
#ifdef CONFIG_MODULES
bool try_module_get(struct module *owner);
#else
static inline bool try_module_get(struct module *owner) { return owner==NULL; }
#endif

/**
 * @brief Release one acquired external reference; IRQ-safe and never invokes module code.
 * @param owner Referenced owner; NULL is a no-op. The caller must own one reference.
 */
#ifdef CONFIG_MODULES
void module_put(struct module *owner);
#else
static inline void module_put(struct module *owner) { (void)owner; }
#endif

/** @brief Find a LIVE module and acquire one external reference under the registry lock.
 * @param name Terminated normalized module name; hyphens match underscores.
 * @return Owned reference or NULL; caller releases with module_put. IRQ-safe.
 */
#ifdef CONFIG_MODULES
struct module *module_get_by_name(const char *name);
#else
static inline struct module *module_get_by_name(const char *name) { (void)name; return NULL; }
#endif
/** @brief Pin a callback before entering or queueing LIVE module code; IRQ-safe.
 * @param owner Stable borrowed registration owner; NULL is permanent built-in code.
 * @return True when pinned; false while COMING/GOING. Caller must hold the registration lock.
 */
#ifdef CONFIG_MODULES
bool module_execution_get(struct module *owner);
#else
static inline bool module_execution_get(struct module *owner) { return owner==NULL; }
#endif
/** @brief Release a completed/cancelled callback pin without invoking module code.
 * @param owner Owner of one acquired execution pin; NULL is a no-op. IRQ-safe.
 */
#ifdef CONFIG_MODULES
void module_execution_put(struct module *owner);
#else
static inline void module_execution_put(struct module *owner) { (void)owner; }
#endif
/** @brief Track a self-owned resource without creating a permanent external reference.
 * @param owner Stable COMING/LIVE owner; NULL denotes a permanent built-in resource.
 * @param quiesce Optional task-context STOP/cancel callback; nonzero retains code/resources.
 * @param release Required task-context final release callback, run only after callback drain.
 * @param context Borrowed callback state; remains owned by the registering component until release.
 * @return Zero or negative errno. No callback runs while the module-table lock is held.
 */
#ifdef CONFIG_MODULES
int module_add_resource(struct module *owner,int (*quiesce)(void *),
                        void (*release)(void *),void *context);
#else
static inline int module_add_resource(struct module *owner,int (*quiesce)(void *),
                                      void (*release)(void *),void *context)
{
    (void)quiesce; (void)context;
    return owner ? -38 : (release ? 0 : -22);
}
#endif
/** @brief Read Linux /proc/modules records into a bounded kernel buffer.
 * @param offset Byte offset for short/repeated reads.
 * @param buffer Writable caller-owned bytes; may be NULL only for zero capacity.
 * @param capacity Buffer capacity in bytes.
 * @param written Required result receiving bytes copied.
 * @return Zero or negative errno; snapshots under the registry lock, no I/O or callbacks.
 */
int module_proc_read(uint64_t offset,void *buffer,uint32_t capacity,uint32_t *written);

/** @brief Copy one module's status without exposing a borrowed runtime pointer.
 * @param index Zero-based current registry index; enumeration may change across calls.
 * @param out Required caller-owned snapshot.
 * @return Zero or -ENOENT/-EINVAL; short IRQ-safe registry transaction, no callbacks.
 */
int module_status_at(size_t index,struct module_status *out);
/** @brief Copy one importing module's name for a provider's sysfs holders directory.
 * @param name Terminated provider name from a stable status snapshot.
 * @param index Zero-based importing module index.
 * @param out Required writable MODULE_NAME_LEN-byte name array.
 * @return Zero or -ENOENT/-EINVAL; no external references or pointers escape the lock.
 */
int module_holder_at(const char *name,size_t index,char out[MODULE_NAME_LEN]);

struct module_identity;
/** @brief Retain an immutable module-instance identity without pinning code.
 * @param name Exact terminated name copied from the loaded-module registry.
 * @return One owned identity, or NULL if absent/reference count exhausted.
 * Task context; short registry lock, no allocation, callbacks or I/O. The
 * token outlives unload while referenced and never identifies a reload.
 */
struct module_identity *module_identity_get(const char *name);
/** @brief Check whether a retained instance still exists in the registry.
 * @param identity Borrowed owned token, nullable.
 * @return True until final runtime retirement; false for NULL or unloaded
 * instances, even after the same module name is loaded again. Short lock
 * only; the token does not retain executable sections or module users.
 */
bool module_identity_alive(const struct module_identity *identity);
/** @brief Release one identity after a generated inode or runtime retires.
 * @param identity Owned token, or NULL; ownership is consumed once.
 * Task context; final heap free happens outside the registry lock. No module
 * callback, code reference, namespace lookup or device I/O is involved.
 */
void module_identity_put(struct module_identity *identity);

#endif

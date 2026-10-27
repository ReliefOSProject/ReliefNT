/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_CHRDEV_H
#define RELIEFNT_CHRDEV_H
#include <reliefnt/module.h>
struct device_mapping;
struct iovec;

/* Persistent callbacks use the canonical OFD supplied at each invocation;
 * they must not retain the address of an inline descriptor before promotion.
 * The caller owns execution and validates user addresses/access flags. Each
 * admitted open pins provider code until final close, including partial-open
 * cleanup. No registry lock spans a callback or a sleeping operation. */
struct character_file_operations {
    int (*open)(struct task *task,struct task_file *file);
    void (*close)(struct task_file *file);
    int64_t (*read)(struct task *task,struct task_file *file,void *buffer,uint64_t length);
    int64_t (*write)(struct task *task,struct task_file *file,const void *buffer,uint64_t length);
    int64_t (*ioctl)(struct task *task,struct task_file *file,uint64_t request,uint64_t argument);
    short (*poll)(struct task_file *file,short events);
    int64_t (*seek)(struct task_file *file,int64_t offset,unsigned whence);
    int (*mmap)(struct task *task,struct task_file *file,uint64_t offset,uint64_t length,
                uint32_t prot,uint64_t flags,struct device_mapping **out);
    int64_t (*readv)(struct task *task,struct task_file *file,const struct iovec *vectors,
                     uint64_t count,bool positional,uint64_t position,uint64_t flags);
};
struct character_device_registration {
    uint32_t major,first_minor,count;
    struct module *owner;
    const struct character_file_operations *operations;
};
struct character_file;
/** @brief Register a Linux dev_t character range without a self-reference.
 * @param registration Persistent immutable descriptor, valid through cleanup.
 * @return Zero or negative errno. Task/init context. Overlap is rejected;
 * module-owned registrations join its cleanup ledger, COMING/GOING opens are
 * closed. No allocation or callback under the character registry spinlock.
 */
int character_device_register(const struct character_device_registration *registration);
/** @brief Remove an idle exact registration without entering provider code.
 * @param registration Persistent exact descriptor, required.
 * @return Zero or EINVAL/ENOENT/EBUSY. Task context; active OFDs prevent removal.
 * Caller retains responsibility for hardware STOP and deferred callback drain.
 */
int character_device_unregister(const struct character_device_registration *registration);
/** @brief Admit an exact dev_t and enter its original callback with code pinned.
 * @param device Linux encoded dev_t, bounded to kernel major/minor widths.
 * @param task Borrowed current task.
 * @param file Borrowed OFD passed through without retaining its slot address.
 * @param out Empty handle output, kept empty on failure.
 * @return Zero or negative errno. Execution-owned task context; partial open
 * closes before dropping code, no character/module lock spans any callback.
 */
int character_file_open(uint64_t device,struct task *task,struct task_file *file,struct character_file **out);
/** @brief Consume one final OFD handle after all in-flight file users drain.
 * @param handle Owned admitted handle, nullable.
 * @param file Current canonical OFD, borrowed through the callback.
 * Execution-owned task context; close completes before provider pin release.
 */
void character_file_close(struct character_file *handle,struct task_file *file);
/** @brief Dispatch read through an OFD-pinned provider.
 * @param handle Borrowed admitted handle.
 * @param task Borrowed current task.
 * @param file Borrowed current canonical OFD.
 * @param buffer Caller-validated user/kernel destination, according to backend.
 * @param length Requested byte bound.
 * @return Backend bytes/errno, EINVAL when absent. Execution ownership required;
 * handle retained by caller, no registry locks or ownership transfer.
 */
int64_t character_file_read(struct character_file *handle,struct task *task,struct task_file *file,void *buffer,uint64_t length);
/** @brief Dispatch write through the retained provider.
 * @param handle Borrowed admitted handle.
 * @param task Borrowed current task.
 * @param file Borrowed canonical OFD.
 * @param buffer Caller-validated input.
 * @param length Requested bytes.
 * @return Backend bytes/errno, EINVAL when absent. Execution ownership required;
 * no registry lock, allocation or new provider reference in the adapter.
 */
int64_t character_file_write(struct character_file *handle,struct task *task,struct task_file *file,const void *buffer,uint64_t length);
/** @brief Dispatch ioctl while the original OFD keeps provider code mapped.
 * @param handle Borrowed admitted handle.
 * @param task Current task, borrowed.
 * @param file Canonical OFD, borrowed.
 * @param request Linux ioctl selector.
 * @param argument Backend-validated ioctl argument.
 * @return Backend result or ENOTTY when absent. Execution ownership required;
 * no registry lock or reference transfer across the backend callback.
 */
int64_t character_file_ioctl(struct character_file *handle,struct task *task,struct task_file *file,uint64_t request,uint64_t argument);
/** @brief Poll an admitted OFD without reacquiring a potentially reused dev_t.
 * @param handle Borrowed admitted handle.
 * @param file Borrowed canonical OFD.
 * @param events Requested readiness bits.
 * @return Backend readiness or standard default readable/writable mask filtered
 * by events. Execution ownership required; no registry lock or reference change.
 */
short character_file_poll(struct character_file *handle,struct task_file *file,short events);
/** @brief Seek an admitted OFD using its own provider's policy.
 * @param handle Borrowed admitted handle.
 * @param file Canonical OFD, borrowed.
 * @param offset Signed requested position.
 * @param whence Linux seek selector.
 * @return New offset or errno, ESPIPE when unsupported. Execution ownership
 * required; handle retained by caller, no registry lock or ownership transfer.
 */
int64_t character_file_seek(struct character_file *handle,struct task_file *file,int64_t offset,unsigned whence);
/** @brief Acquire a mapping with backing/code lifetime independent of its OFD.
 * @param handle Borrowed admitted provider handle.
 * @param task Current task. @param file Canonical OFD.
 * @param offset Backend mapping selector/offset. @param length Rounded extent.
 * @param prot Requested protection. @param flags Linux mmap flags.
 * @param out Empty output receiving one owned mapping reference.
 * @return Zero or backend/ENODEV errno. Execution ownership required; provider
 * remains pinned during construction, and output stays empty on failure.
 * No character/module registry lock spans callbacks or allocation.
 */
int character_file_mmap(struct character_file *handle,struct task *task,struct task_file *file,
                         uint64_t offset,uint64_t length,uint32_t prot,uint64_t flags,struct device_mapping **out);
/** @brief Query whether this retained provider handles vectored reads itself.
 * @param handle Borrowed admitted handle, required.
 * @return True exactly when readv is present. Execution-owned pure snapshot;
 * no allocation, lock, callback or change in provider ownership.
 */
bool character_file_has_readv(const struct character_file *handle);
/** @brief Delegate imported vectors to their original OFD-pinned provider.
 * @param handle Borrowed admitted handle. @param task Current task.
 * @param file Canonical OFD. @param vectors Borrowed validated kernel iovecs.
 * @param count Vector count. @param positional Use the explicit offset.
 * @param position Explicit offset, ignored when nonpositional.
 * @param flags Native Linux RWF flags, validated by this backend.
 * @return Bytes/backend errno, or EOPNOTSUPP when absent. Execution ownership
 * required; no registry lock spans the callback, and no reference is transferred.
 */
int64_t character_file_readv(struct character_file *handle,struct task *task,struct task_file *file,
                             const struct iovec *vectors,uint64_t count,bool positional,
                             uint64_t position,uint64_t flags);
#endif

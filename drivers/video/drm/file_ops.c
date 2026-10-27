/* SPDX-License-Identifier: Apache-2.0 */
#include "file_ops.h"
#include "services.h"
#include <reliefnt/chrdev.h>
#include <reliefnt/drm.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <linux/errno.h>
#include <linux/uio.h>

/** @brief Open the actual card/render inode or adopt already owned PRIME state.
 * @param task Current execution-owned caller. @param file Borrowed unpublished OFD.
 * @return Backend errno or zero. Original provider is pinned before entry;
 * partial state is closed by the generic character adapter before code unpin.
 */
static int drm_character_open(struct task *task,struct task_file *file)
{return file->drm_prime_file ? 0 : drm_file_open(task,file);}
/** @brief Release card and PRIME resources before dropping the provider pin.
 * @param file Borrowed canonical OFD, possibly a partially failed open.
 * Execution-owned task context; backend retirements retain their own lifetime.
 * No character registry lock spans backend close or resource queuing.
 */
static void drm_character_close(struct task_file *file)
{drm_prime_file_close(file);drm_file_close(file);}
/** @brief Read card events or apply the opaque PRIME object's stream policy.
 * @param task Current caller. @param file Original retained OFD.
 * @param buffer Validated user destination. @param length Requested byte count.
 * @return Bytes/errno or internal blocked status. Execution ownership required;
 * no registry lock spans the DRM wait/copyout and no references are transferred.
 */
static int64_t drm_character_read(struct task *task,struct task_file *file,void *buffer,uint64_t length)
{return file->drm_prime_file ? drm_prime_file_rw(file,false) : drm_event_read(task,file,(uintptr_t)buffer,length);}
/** @brief Preserve card/opaque PRIME write access and unsupported-data policy.
 * @param task Current caller. @param file Retained OFD.
 * @param buffer Validated input. @param length Requested bytes.
 * @return EBADF when access is denied, otherwise EINVAL. Execution-owned;
 * pure validation with no allocation, callback or reference transfer.
 */
static int64_t drm_character_write(struct task *task,struct task_file *file,const void *buffer,uint64_t length)
{
    (void)task;(void)buffer;(void)length;
    return file->drm_prime_file ? drm_prime_file_rw(file,true) : file_can_write(file) ? -EINVAL : -EBADF;
}
/** @brief Dispatch DRM or DMA-BUF ioctl after generic VFS ioctl handling.
 * @param task Caller used for usercopy. @param file Retained OFD.
 * @param request Linux low32bit selector. @param argument Backend-validated operand.
 * @return Backend result/errno. Execution-owned, provider code remains pinned;
 * no registry lock spans ioctl, wait or copyout.
 */
static int64_t drm_character_ioctl(struct task *task,struct task_file *file,uint64_t request,uint64_t argument)
{return drm_fd_ioctl(task,file,(uint32_t)request,argument);}
/** @brief Query the retained card/opaque PRIME readiness implementation.
 * @param file Canonical OFD. @param events Requested poll bits.
 * @return Actual readiness; execution-owned, no lock or ownership transfer.
 */
static short drm_character_poll(struct task_file *file,short events)
{return (short)(file->drm_prime_file ? drm_prime_file_poll(file,(uint16_t)events) : drm_file_poll(file,(uint16_t)events));}
/** @brief Preserve Linux DRM noop_llseek and opaque DMA-BUF SET/END behavior.
 * @param file Canonical retained OFD. @param offset Requested offset.
 * @param whence Linux seek selector.
 * @return Unchanged card f_pos, PRIME size/zero, or errno. Execution-owned;
 * no allocation, backend wait, reference transfer or registry lock.
 */
static int64_t drm_character_seek(struct task_file *file,int64_t offset,unsigned whence)
{
    if(file->drm_prime_file)return drm_prime_file_llseek(file,offset,whence);
    if(!file->drm_file)return -EBADF;
    return whence>4u ? -EINVAL : (int64_t)file->offset;
}
/** @brief Delegate card vectors while retaining opaque PRIME's byte-I/O policy.
 * @param task Caller. @param file Original OFD. @param vectors Imported array.
 * @param count Vector count. @param positional Select explicit offset semantics.
 * @param position Explicit offset. @param flags Linux RWF flags.
 * @return Bytes/backend errno. Execution-owned, borrowed vectors and pinned code;
 * no registry lock spans event waits or user copying.
 */
static int64_t drm_character_readv(struct task *task,struct task_file *file,const struct iovec *vectors,
                                  uint64_t count,bool positional,uint64_t position,uint64_t flags)
{
    if(!file->drm_prime_file)return drm_event_readv(task,file,vectors,count,positional,position,flags);
    if(flags)return -EOPNOTSUPP;
    for(uint64_t i=0;i<count;++i)if(vectors[i].iov_len)return drm_prime_file_rw(file,false);
    return 0;
}
static const struct character_file_operations drm_character_operations={
    .open=drm_character_open,.close=drm_character_close,.read=drm_character_read,
    .write=drm_character_write,.ioctl=drm_character_ioctl,.poll=drm_character_poll,
    .seek=drm_character_seek,.readv=drm_character_readv,.mmap=drm_character_mmap};
static const struct character_device_registration drm_character_registration={
    .major=226,.count=256,.owner=THIS_MODULE,.operations=&drm_character_operations};
/** @brief Acquire the exact DRM provider for an already constructed PRIME OFD.
 * @param task Execution-owned caller. @param file Unpublished PRIME OFD.
 * @return Zero or errno, new handle owned by file on success; on failure the
 * generic callback may have already closed PRIME state. No registry lock spans
 * allocation/callbacks; provider reference precedes fd publication/promotion.
 */
int drm_character_attach_prime(struct task *task,struct task_file *file)
{return character_file_open(226u<<8,task,file,&file->character_file);}
/** @brief Publish DRM's standard major after the heap and core registries exist.
 * @return Zero or registration errno. Init task context, no self module pin;
 * framebuffer discovery is copied here; no early optional DRM call is needed.
 */
static int drm_character_init(void)
{
    int rc=character_device_register(&drm_character_registration);if(rc)return rc;
    rc=drm_core_services_start();
    if(rc)(void)character_device_unregister(&drm_character_registration);
    return rc;
}
/** @brief Close admission after the last OFD has released its provider pin.
 * Task context; the module ledger may already have unlinked the exact range.
 * No hardware operation, allocation or graph/module lock occurs here.
 */
static void drm_character_exit(void)
{drm_core_services_stop();(void)character_device_unregister(&drm_character_registration);}
module_init(drm_character_init);
module_exit(drm_character_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT DRM/KMS and VMware SVGA provider");
MODULE_ALIAS("pci:v000015ADd00000405sv*sd*bc*sc*i*");

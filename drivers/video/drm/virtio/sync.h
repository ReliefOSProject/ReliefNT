/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_VIRTGPU_SYNC_H
#define RELIEFNT_VIRTGPU_SYNC_H
#include <reliefnt/types.h>
struct task;
struct task_file;
struct vg_fence;
/** @brief Allocate a refcounted GPU completion, retaining module code.
 * @return One owned pending fence or NULL. Execution-owned task context.
 */
struct vg_fence *vg_fence_create(void);
/** @brief Publish one final DMA completion before dropping submission ownership.
 * @param fence Nullable owned completion. @param status Zero success or errno.
 * Execution-owned; no IRQ callback may release resources through this function.
 */
void vg_fence_signal(struct vg_fence *fence,int status);
/** @brief Drop one owned fence reference; free only after its last observer.
 * @param fence Nullable retained fence. Execution-owned task context.
 */
void vg_fence_put(struct vg_fence *fence);
/** @brief Publish a CLOEXEC Linux sync_file holding its own fence reference.
 * @param task Caller. @param fence Borrowed completion. @param fd Output.
 * @return Zero or errno; caller rolls back the returned fd on copyout failure.
 */
int vg_sync_export(struct task *task,struct vg_fence *fence,int *fd);
/** @brief Validate and wait for every completion in an imported sync_file.
 * @param task Caller. @param fd Input sync_file descriptor.
 * @return Zero, negative errno or scheduler blocked marker; no lock spans sleep.
 */
int vg_sync_wait_fd(struct task *task,int fd);
/** @brief Consume final canonical sync_file state before provider code unpins.
 * @param file Closing OFD. Execution-owned; tolerant of partial open failure.
 */
void vg_sync_close(struct task_file *file);
/** @brief Query actual terminal fence status for poll/epoll/select.
 * @param file Retained OFD. @param events Requested bits.
 * @return POLLIN only after all work terminates, including failed work.
 */
short vg_sync_poll(struct task_file *file,short events);
/** @brief Implement the Linux sync_file MERGE, FILE_INFO and deadline hint ABI.
 * @param task Caller. @param file Provider-pinned canonical OFD.
 * @param command Ioctl selector. @param argument User argument address.
 * @return Zero, errno or prerequisite retry; outputs prepared before fd mutation.
 */
int64_t vg_sync_ioctl(struct task *task,struct task_file *file,uint32_t command,uint64_t argument);
#endif

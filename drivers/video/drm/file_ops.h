/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DRM_FILE_OPS_H
#define RELIEFNT_DRM_FILE_OPS_H
#include <reliefnt/types.h>
struct task;
struct task_file;
struct iovec;
struct device_mapping;
/** @brief Read a bounded slice of DRM events through an admitted OFD.
 * @param task Execution-owned caller. @param file Borrowed DRM description.
 * @param buffer Writable user address. @param length Requested bytes.
 * @return Bytes/errno or internal blocking status. Caller retains its OFD;
 * no registry lock spans copyout/wait, and no references are transferred.
 */
int64_t drm_event_read(struct task *task,struct task_file *file,uint64_t buffer,uint64_t length);
/** @brief Apply DRM event-stream semantics to imported, borrowed vectors.
 * @param task Execution-owned caller. @param file Admitted DRM OFD.
 * @param vectors Validated kernel array. @param count Number of vectors.
 * @param positional Reject negative explicit offsets while leaving f_pos alone.
 * @param position Explicit byte offset. @param flags Zero or Linux RWF_HIPRI.
 * @return Bytes/backend errno. No registry lock or ownership transfer;
 * caller retains its OFD through blocking/retry and revalidates user memory.
 */
int64_t drm_event_readv(struct task *task,struct task_file *file,const struct iovec *vectors,
                       uint64_t count,bool positional,uint64_t position,uint64_t flags);
/** @brief Bind an unpublished PRIME OFD to the same retained DRM operation table.
 * @param task Execution-owned caller. @param file OFD already owning PRIME state.
 * @return Zero or errno. Success transfers a new provider handle to file;
 * failure leaves the handle empty and may have already closed partial state.
 * No registry lock spans callbacks or allocation. Caller rolls back its FD.
 */
int drm_character_attach_prime(struct task *task,struct task_file *file);
/** @brief Acquire one BO/OFD lease behind a provider-pinned generic mapping.
 * @param task Execution-owned caller. @param file Original retained OFD.
 * @param offset DRM BO selector/offset. @param length Rounded mapping bytes.
 * @param prot Requested PROT bits. @param flags Linux mmap flags.
 * @param out Empty output receiving one owned backing reference.
 * @return Zero or errno; unpublished failures release BO/OFD/context once.
 * Card mappings are shared, nonexecutable and nonexpandable, as Linux TTM;
 * opaque PRIME retains its unsupported-mmap access/error policy. No registry
 * lock spans allocation, callback or the final OFD put.
 */
int drm_character_mmap(struct task *task,struct task_file *file,uint64_t offset,uint64_t length,
                       uint32_t prot,uint64_t flags,struct device_mapping **out);
#endif

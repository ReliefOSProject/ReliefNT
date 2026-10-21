/* Private SysV shared-memory ownership hooks; no public ABI mirror. */
#ifndef RELIEFNT_SYSV_SHM_H
#define RELIEFNT_SYSV_SHM_H
#include <reliefnt/types.h>
struct task;
struct task_vma;
struct sysv_shm_attachment;

/** @brief Execute a native shared-memory syscall under execution ownership.
 * @param number Native syscall number. @param a0 First raw argument.
 * @param a1 Second raw argument. @param a2 Third raw argument.
 * @return Native address/ID, zero, or negative errno; no wait or module callback.
 */
int64_t syscall_sysv_shm(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2);
/** @brief Retain the same attachment when an existing VMA is split.
 * @param vma Copied VMA with a borrowed attachment pointer, under execution ownership.
 * @return None. Increments piece and Linux nattch counts, not physical PTE refs.
 */
void sysv_shm_vma_split(struct task_vma *vma);
/** @brief Acquire a child's attachment while copying one VMA during fork.
 * @param child New independent MM owner. @param dst Copied VMA with no owned reference.
 * @param src Stable parent VMA; NULL attachment is a successful no-op.
 * @return Zero or -ENOMEM; failure leaves dst without an attachment reference.
 */
int sysv_shm_vma_clone(struct task *child, struct task_vma *dst, const struct task_vma *src);
/** @brief Release one VMA piece after its PTE references are unmapped or separately retained.
 * @param task MM owner for last-operator PID; may be NULL in cleanup.
 * @param vma Owned piece under execution ownership; pointer is cleared exactly once.
 * @return None. Final RMID piece releases registry pages and attachment storage.
 */
void sysv_shm_vma_release(struct task *task, struct task_vma *vma);
/** @brief Map retained shared pages through the existing VMA/page-table manager.
 * @param task Current user task. @param address Exact address, or zero for allocation.
 * @param length Page-rounded nonzero bytes. @param prot Initial Linux protection.
 * @param max_prot Maximum protection. @param remap Replace exact existing ranges.
 * @param attachment Prepared identity, published only on success; borrowed here.
 * @param pages Registry-owned page vector. Every mapped PTE acquires its own reference.
 * @return User address or negative errno. Failure releases all acquired PTE references.
 */
int64_t syscall_mm_map_sysv_shm(struct task *task, uint64_t address, uint64_t length,
    uint32_t prot, uint32_t max_prot, bool remap,
    struct sysv_shm_attachment *attachment, const uint64_t *pages);
#endif

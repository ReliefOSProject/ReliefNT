/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "../../block/core/storage_internal.h"

/* Weak declarations make missing claim behavior a runtime failure. */
extern int storage_inode_swap_claim(struct storage_inode_ref *) __attribute__((weak));
extern void storage_inode_swap_release(struct storage_inode_ref *) __attribute__((weak));
extern bool storage_inode_swap_active(const struct storage_inode_ref *) __attribute__((weak));

/** @brief Model recursive execution ownership in this single-threaded fixture.
 * @param flags Required saved state output.
 */
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags=0; }
/** @brief Release fixture execution ownership. @param flags Saved state. */
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }

/** @brief Verify a pre-existing writable description differs from active writes.
 * @return Zero after claim/release and original writer ownership balance.
 */
int main(void)
{
    if(!storage_inode_swap_claim || !storage_inode_swap_release || !storage_inode_swap_active) {
        fputs("FAIL: production inode swap claim is absent\n",stderr); return 1;
    }
    struct storage_inode_ref inode={.node={.type=RELIEFOS_FS_TYPE_FILE},.references=1};
    assert(!storage_inode_write_get(&inode) && inode.write_count==1);
    assert(!storage_inode_swap_claim(&inode) && storage_inode_swap_active(&inode));
    assert(inode.write_count==1);
    assert(storage_inode_swap_claim(&inode)==-EBUSY);
    /* Linux permits a later plain writable open; mutation checks are dynamic. */
    assert(!storage_inode_write_get(&inode) && inode.write_count==2);
    assert(storage_inode_deny_write(&inode)==-ETXTBSY);
    storage_inode_write_put(&inode);storage_inode_write_put(&inode);assert(inode.write_count==0);
    storage_inode_swap_release(&inode);
    assert(!storage_inode_swap_active(&inode) && !storage_inode_write_get(&inode));
    storage_inode_write_put(&inode);
    assert(!storage_inode_deny_write(&inode));
    assert(storage_inode_swap_claim(&inode)==-ETXTBSY);
    storage_inode_allow_write(&inode);
    inode.read_only=true; assert(storage_inode_swap_claim(&inode)==-EROFS);
    puts("swap inode claim: ok"); return 0;
}

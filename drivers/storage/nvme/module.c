/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_nvme.h"
#include <reliefnt/module.h>
/** @brief Register the NVMe transport before root probing or during module init.
 * @return Zero for absent/successful hardware or native error with owned rollback.
 * Execution ownership required; probes publish block devices without filesystem
 * callbacks and native cleanup retains queues/code when controller STOP fails.
 */
static int nvme_init(void) {return nvme_probe_all();}
/** @brief Finish module exit after fallible controller/registration cleanup drained.
 * No provider pointer remains admitted; owner ledger performs final resources.
 */
static void nvme_exit(void) { }
block_initcall(nvme_init);
module_exit(nvme_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT NVMe block transport");
MODULE_ALIAS("pci:v*d*sv*sd*bc01sc08i02*");

/* SPDX-License-Identifier: Apache-2.0 */
#include "common.h"
static int busy_init(void) { return TEST_CONTROL(3); }
static void busy_exit(void) { (void)TEST_CONTROL(13); }
module_init(busy_init);
module_exit(busy_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT module quiescence test");
#include "metadata.h"

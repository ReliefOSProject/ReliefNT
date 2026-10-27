/* SPDX-License-Identifier: Apache-2.0 */
#include "common.h"
#include <reliefnt/moduleparam.h>
static int failure=-22;
module_param(failure,int,0);
static int failing_init(void) { int rc=TEST_CONTROL(2); return rc ? rc : failure; }
static void failing_exit(void) { (void)TEST_CONTROL(12); }
module_init(failing_init);
module_exit(failing_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT module failed-init cleanup test");
#include "metadata.h"

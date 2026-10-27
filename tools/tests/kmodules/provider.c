/* SPDX-License-Identifier: Apache-2.0 */
#include "common.h"
int provided_value(void);
int provided_value(void) { return 42; }
EXPORT_SYMBOL(provided_value);
static int provider_init(void) { return TEST_CONTROL(0); }
static void provider_exit(void) { (void)TEST_CONTROL(10); }
module_init(provider_init);
module_exit(provider_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT module dependency test provider");
#define TEST_PROVIDER
#include "metadata.h"

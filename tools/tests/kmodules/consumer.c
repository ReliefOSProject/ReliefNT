/* SPDX-License-Identifier: Apache-2.0 */
#include "common.h"
extern int provided_value(void);
static int consumer_init(void) { return provided_value()==42 ? TEST_CONTROL(1) : -22; }
static void consumer_exit(void) { (void)TEST_CONTROL(11); }
module_init(consumer_init);
module_exit(consumer_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT module dependency test consumer");
#define TEST_CONSUMER
#include "metadata.h"

/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/module.h>
#include <reliefnt/export.h>
#ifdef RELIEFNT_HOST_MODULE_FIXTURE
extern int test_control(struct module *owner,int stage);
#define TEST_CONTROL(stage) test_control(THIS_MODULE,stage)
#else
#define TEST_CONTROL(stage) (0)
#endif

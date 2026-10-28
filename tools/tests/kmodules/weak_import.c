/* SPDX-License-Identifier: Apache-2.0 */
/* Host loader fixture; the production Kbuild test modules remain unchanged. */
#include "common.h"
#ifndef TEST_WEAK_SYMBOL
#define TEST_WEAK_SYMBOL absent_optional_value
#endif
#ifndef TEST_WEAK_LICENSE
#define TEST_WEAK_LICENSE "Apache-2.0"
#endif
/** @brief Optional fixture import, present providers return the literal 42.
 * @return 42 from the host provider; a missing weak definition has address zero.
 */
extern int TEST_WEAK_SYMBOL(void) __attribute__((weak));
/** @brief Admit absent optional imports while checking an actually bound export.
 * @return Zero for absence or the expected value, -EINVAL for a wrong provider.
 */
static int weak_import_init(void)
{
    return TEST_WEAK_SYMBOL && TEST_WEAK_SYMBOL()!=42 ? -22 : 0;
}
/** @brief Finish an owner-free host fixture; no device or callback was registered. */
static void weak_import_exit(void) { }
module_init(weak_import_init);
module_exit(weak_import_exit);
MODULE_LICENSE(TEST_WEAK_LICENSE);
#ifdef TEST_WEAK_IMPORT_NS
MODULE_IMPORT_NS(RELIEFNT_WEAK_TEST);
#endif
#include "metadata.h"

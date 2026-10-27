/* SPDX-License-Identifier: Apache-2.0 */
#include "common.h"
#include <reliefnt/moduleparam.h>
static bool enabled;
static int signed_value;
static unsigned int unsigned_value;
static char *label;
static char *runtime_label="initial";
module_param(enabled,bool,0644);
module_param(signed_value,int,0444);
module_param(unsigned_value,uint,0444);
module_param(label,charp,0);
module_param(runtime_label,charp,0644);
/** @brief Check quoted module-load parameters before publishing the fixture.
 * @return Zero or EINVAL; private initialization, no locks or allocations.
 */
static int parameters_init(void)
{
    if(!enabled || signed_value!=(-2147483647-1) || unsigned_value!=4294967295u || !label)
        return -22;
    static const char expected[]="hello world";
    for(unsigned i=0;i<sizeof(expected);++i) if(label[i]!=expected[i]) return -22;
    return 0;
}
/** @brief Stop the fixture after all users drain; framework owns strings. */
static void parameters_exit(void) { }
module_init(parameters_init);
module_exit(parameters_exit);
MODULE_LICENSE("Apache-2.0");
#include "metadata.h"

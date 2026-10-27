/* SPDX-License-Identifier: Apache-2.0 */
/* No imported functions: exercises real high-address code, RO data and BSS. */
#include <reliefnt/module.h>
static volatile unsigned zeroed[8193];
static const volatile unsigned marker=0x1234abcd;
static int standalone_init(void)
{
    for(unsigned i=0;i<8193;++i) if(zeroed[i]) return -22;
    if(marker!=0x1234abcd) return -22;
    zeroed[8192]=42;
    return 0;
}
static void standalone_exit(void) { zeroed[8192]=0; }
module_init(standalone_init);
module_exit(standalone_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("Native module address-space and page-permission probe");
#ifdef RELIEFNT_TEST_LINK
/* Early loader checkpoint; final Kbuild tests use original modpost output. */
MODULE_INFO(name,KBUILD_MODNAME);
MODULE_INFO(vermagic,RELIEFNT_MODULE_VERMAGIC);
struct module __this_module __attribute__((used,section(".gnu.linkonce.this_module"))) = {
    .name=KBUILD_MODNAME,.init=init_module,.exit=cleanup_module,
};
#endif

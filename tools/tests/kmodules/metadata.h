/* SPDX-License-Identifier: Apache-2.0 */
/* Host-only final-link fixture: K4 tests use original modpost instead. */
#ifdef RELIEFNT_HOST_MODULE_FIXTURE
#include "../../../kernel/module/internal.h"
#ifndef TEST_VERMAGIC
#define TEST_VERMAGIC "reliefnt-test-abi-1"
#endif
#ifndef TEST_LAYOUT_CRC
#define TEST_LAYOUT_CRC 0x55667788u
#endif
#ifndef TEST_IMPORT_CRC
#define TEST_IMPORT_CRC 0x11223344u
#endif
MODULE_INFO(name,KBUILD_MODNAME);
MODULE_INFO(vermagic,TEST_VERMAGIC);
struct module __this_module __attribute__((section(".gnu.linkonce.this_module"),used)) = {
    .name=KBUILD_MODNAME,.init=init_module,.exit=cleanup_module,
};
static const struct modversion_info versions[] __attribute__((section("__versions"),used)) = {
    {TEST_LAYOUT_CRC,"module_layout"},
    {TEST_IMPORT_CRC,"test_control"},
#ifdef TEST_CONSUMER
    {0x12345678u,"provided_value"},
#endif
};
#ifdef TEST_PROVIDER
static const struct module_export_symbol exports[] __attribute__((section("__ksymtab"),used)) = {
    {(uintptr_t)provided_value,"provided_value",""},
};
static const uint32_t export_crcs[] __attribute__((section("__kcrctab"),used)) = {0x12345678u};
#endif
#endif

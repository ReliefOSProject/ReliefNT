/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_UAPI_LINUX_MODULE_H
#define RELIEFNT_UAPI_LINUX_MODULE_H
/* Linux finit_module(2) flag numbers. ReliefNT never bypasses ABI checks. */
#define MODULE_INIT_IGNORE_MODVERSIONS 1
#define MODULE_INIT_IGNORE_VERMAGIC 2
#define MODULE_INIT_COMPRESSED_FILE 4
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MODULE_COMPILER_ADAPTER_H
#define RELIEFNT_MODULE_COMPILER_ADAPTER_H
/* Only the metadata emitted by original modpost consumes these spellings. */
#define __visible __attribute__((visibility("default")))
#define __used __attribute__((used))
#define __section(name) __attribute__((section(name)))
#endif

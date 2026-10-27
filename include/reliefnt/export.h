/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_EXPORT_H
#define RELIEFNT_EXPORT_H

#include <reliefnt/module.h>

/* Linux 6.12 modpost input: two strings, natural alignment, absolute symbol. */
#ifdef __GENKSYMS__
#define __RN_EXPORT(symbol, license, namespace) __GENKSYMS_EXPORT_SYMBOL(symbol)
#else
#define __RN_EXPORT(symbol, license, namespace) \
    __asm__(".pushsection .export_symbol,\"a\",@progbits\n" \
            "__export_symbol_" #symbol ":\n" \
            ".asciz \"" license "\"\n.asciz \"" namespace "\"\n" \
            ".balign 8\n.quad " #symbol "\n.popsection\n")
#endif
#define EXPORT_SYMBOL(symbol) __RN_EXPORT(symbol, "", "")
#define EXPORT_SYMBOL_GPL(symbol) __RN_EXPORT(symbol, "GPL", "")
#define EXPORT_SYMBOL_NS(symbol, namespace) __RN_EXPORT(symbol, "", __RN_STRING(namespace))
#define EXPORT_SYMBOL_NS_GPL(symbol, namespace) __RN_EXPORT(symbol, "GPL", __RN_STRING(namespace))

#endif

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MODULE_EXPORT_METADATA_ADAPTER_H
#define RELIEFNT_MODULE_EXPORT_METADATA_ADAPTER_H
/* Native absolute-pointer tables retain Linux's section and per-export labels
 * so module tools can discover exports without decoding runtime relocations.
 * Functional metadata only; there is no Linux internal runtime API here. */
#define __RN_KSYMTAB(symbol, suffix, space) \
    __asm__(".pushsection __ksymtab_strings,\"a\",@progbits\n" \
            "__kstrtab_" #symbol ": .asciz \"" #symbol "\"\n" \
            "__kstrtabns_" #symbol ": .asciz \"" space "\"\n.popsection\n" \
            ".pushsection ___ksymtab" suffix "+" #symbol ",\"a\",@progbits\n" \
            ".balign 8\n__ksymtab_" #symbol ":\n.quad " #symbol \
            "\n.quad __kstrtab_" #symbol \
            "\n.quad __kstrtabns_" #symbol "\n.popsection\n")
#define KSYMTAB_FUNC(symbol, suffix, space) __RN_KSYMTAB(symbol, suffix, space)
#define KSYMTAB_DATA(symbol, suffix, space) __RN_KSYMTAB(symbol, suffix, space)
#define SYMBOL_CRC(symbol, crc, suffix) \
    __asm__(".pushsection ___kcrctab" suffix "+" #symbol ",\"a\",@progbits\n" \
            ".balign 4\n__crc_" #symbol ": .long " #crc "\n.popsection\n")
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MODULE_EXPORT_METADATA_ADAPTER_H
#define RELIEFNT_MODULE_EXPORT_METADATA_ADAPTER_H
/* Native absolute-pointer tables with the standard modpost section names.
 * Functional metadata only; there is no Linux internal runtime API here. */
#define __RN_KSYMTAB(symbol, suffix, space) \
    __asm__(".pushsection __ksymtab_strings,\"a\",@progbits\n" \
            ".Lrn_name_" #symbol ": .asciz \"" #symbol "\"\n" \
            ".Lrn_space_" #symbol ": .asciz \"" space "\"\n.popsection\n" \
            ".pushsection ___ksymtab" suffix "+" #symbol ",\"a\",@progbits\n" \
            ".balign 8\n.quad " #symbol "\n.quad .Lrn_name_" #symbol \
            "\n.quad .Lrn_space_" #symbol "\n.popsection\n")
#define KSYMTAB_FUNC(symbol, suffix, space) __RN_KSYMTAB(symbol, suffix, space)
#define KSYMTAB_DATA(symbol, suffix, space) __RN_KSYMTAB(symbol, suffix, space)
#define SYMBOL_CRC(symbol, crc, suffix) \
    __asm__(".pushsection ___kcrctab" suffix "+" #symbol ",\"a\",@progbits\n" \
            ".balign 4\n__crc_" #symbol ": .long " #crc "\n.popsection\n")
#endif

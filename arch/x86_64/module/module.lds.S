/* SPDX-License-Identifier: Apache-2.0 */
/* Native ELF64 ET_REL metadata layout consumed by original Makefile.modfinal. */
SECTIONS
{
    /DISCARD/ : { *(.export_symbol) *(.note.GNU-stack) *(.comment) }
    __ksymtab 0 : { *(SORT(___ksymtab+*)) }
    __ksymtab_gpl 0 : { *(SORT(___ksymtab_gpl+*)) }
    __kcrctab 0 : { *(SORT(___kcrctab+*)) }
    __kcrctab_gpl 0 : { *(SORT(___kcrctab_gpl+*)) }
}

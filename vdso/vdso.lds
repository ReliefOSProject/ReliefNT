OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(__vdso_clock_gettime)

PHDRS
{
	text PT_LOAD FILEHDR PHDRS FLAGS(5);
	dynamic PT_DYNAMIC FLAGS(4);
	stack PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
	. = SIZEOF_HEADERS;
	.hash : { *(.hash) } :text
	.gnu.hash : { *(.gnu.hash) } :text
	.dynsym : { *(.dynsym) } :text
	.dynstr : { *(.dynstr) } :text
	.gnu.version : { *(.gnu.version) } :text
	.gnu.version_d : { *(.gnu.version_d) } :text
	.dynamic : { *(.dynamic) } :text :dynamic
	.text : { *(.text .text.*) } :text
	.rodata : { *(.rodata .rodata.*) } :text

	/DISCARD/ : {
		*(.comment .note .note.* .eh_frame .eh_frame_hdr)
		*(.data .data.* .bss .bss.* .tdata .tbss)
		*(.got .got.* .got.plt .plt .plt.*)
	}

	HIDDEN(__vvar_data = -4096);
}

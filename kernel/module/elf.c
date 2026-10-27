/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Check an image byte interval using subtraction, avoiding end wrap.
 * @param size Readable image size.
 * @param offset Interval start.
 * @param length Interval byte count.
 * @return True only when the complete interval is readable; no locks.
 */
static bool image_range(size_t size,uint64_t offset,uint64_t length)
{
    return offset<=size && length<=size-offset;
}
/** @brief Locate a terminated string entirely within a validated string table.
 * @param view Immutable borrowed view.
 * @param table Validated string-table header.
 * @param offset String offset within table.
 * @return Borrowed string or NULL; never scans outside the image.
 */
static const char *elf_string(const struct module_elf *view,
                              const struct module_elf_section *table,uint32_t offset)
{
    if(offset>=table->size || !image_range(view->size,table->offset,table->size)) return NULL;
    const char *start=(const char *)view->image+table->offset+offset;
    for(uint64_t i=offset;i<table->size;++i) if(!start[i-offset]) return start;
    return NULL;
}
/** @brief Copy one potentially unaligned section header after validating its index.
 * @param view Stable view whose header is already checked.
 * @param index Section-table index.
 * @param out Writable aligned copy.
 * @return Zero or -ENOEXEC; no allocations or ownership transfer.
 */
int module_elf_get_section(const struct module_elf *view,unsigned int index,
                           struct module_elf_section *out)
{
    if(!view || !out || index>=view->header.shnum) return -ENOEXEC;
    uint64_t offset=view->header.shoff+(uint64_t)index*sizeof(*out);
    if(!image_range(view->size,offset,sizeof(*out))) return -ENOEXEC;
    module_copy(out,view->image+offset,sizeof(*out));
    return 0;
}
/** @brief Read one checked ELF64 symbol without assuming input alignment.
 * @param view Stable validated view.
 * @param index Symbol index.
 * @param out Writable aligned copy.
 * @return Zero or -ENOEXEC; caller retains the image.
 */
int module_elf_get_symbol(const struct module_elf *view,size_t index,
                          struct module_elf_symbol *out)
{
    if(!view || !out || index>=view->symbol_count) return -ENOEXEC;
    uint64_t offset=view->symbols.offset+index*sizeof(*out);
    if(!image_range(view->size,offset,sizeof(*out))) return -ENOEXEC;
    module_copy(out,view->image+offset,sizeof(*out));
    return 0;
}
/** @brief Resolve a checked section name within shstrtab.
 * @param view Stable validated view.
 * @param index Section index.
 * @return Borrowed terminated name or NULL, without locks or allocation.
 */
const char *module_elf_section_name(const struct module_elf *view,unsigned int index)
{
    struct module_elf_section section;
    if(module_elf_get_section(view,index,&section)) return NULL;
    return elf_string(view,&view->section_names,section.name);
}
/** @brief Resolve a checked symbol name within strtab.
 * @param view Stable validated view.
 * @param symbol Symbol copy obtained from the view.
 * @return Borrowed terminated name or NULL; no ownership transfer.
 */
const char *module_elf_symbol_name(const struct module_elf *view,const struct module_elf_symbol *symbol)
{
    return view && symbol ? elf_string(view,&view->strings,symbol->name) : NULL;
}
/** @brief Validate all symbol indices, names and defined byte extents.
 * @param view ELF view with checked symbol/string tables.
 * @return Zero or -ENOEXEC; no allocation or side effects.
 */
static int validate_symbols(const struct module_elf *view)
{
    for(size_t i=0;i<view->symbol_count;++i) {
        struct module_elf_symbol symbol;
        if(module_elf_get_symbol(view,i,&symbol) || !module_elf_symbol_name(view,&symbol)) return -ENOEXEC;
        if(!i && (symbol.name || symbol.info || symbol.other || symbol.section || symbol.value || symbol.size))
            return -ENOEXEC;
        if((symbol.info>>4)>2 || (symbol.info&15)==6 || symbol.other>3) return -ENOEXEC;
        if(symbol.section==MODULE_ELF_UNDEF || symbol.section==MODULE_ELF_ABS) continue;
        if(symbol.section>=view->header.shnum) return -ENOEXEC;
        struct module_elf_section section;
        if(module_elf_get_section(view,symbol.section,&section) || symbol.value>section.size ||
           symbol.size>section.size-symbol.value) return -ENOEXEC;
    }
    return 0;
}
/** @brief Check RELA symbols and destination widths before allocating module memory.
 * @param view Validated ELF header and symbol table.
 * @param section RELA section with an already checked file extent.
 * @return Zero or -ENOEXEC; nonallocated debug relocations are bounded but not applied.
 */
static int validate_relocations(const struct module_elf *view,const struct module_elf_section *section)
{
    if(section->entry_size!=sizeof(struct module_elf_rela) ||
       section->size%sizeof(struct module_elf_rela) || section->link!=view->symbol_section ||
       !section->info || section->info>=view->header.shnum) return -ENOEXEC;
    struct module_elf_section target;
    if(module_elf_get_section(view,section->info,&target)) return -ENOEXEC;
    for(uint64_t offset=0;offset<section->size;offset+=sizeof(struct module_elf_rela)) {
        struct module_elf_rela rela;
        module_copy(&rela,view->image+section->offset+offset,sizeof(rela));
        if((rela.info>>32)>=view->symbol_count || rela.offset>target.size) return -ENOEXEC;
        if(target.flags&MODULE_ELF_ALLOC) {
            int width=module_relocation_width((unsigned int)rela.info);
            if(width<0 || (uint64_t)width>target.size-rela.offset) return -ENOEXEC;
        }
    }
    return 0;
}
/** @brief Strictly parse native x86-64 ELF64 ET_REL into an immutable borrowed view.
 * @param image Stable readable kernel-owned bytes, possibly unaligned.
 * @param size Complete image length.
 * @param out Required output, changed only after complete validation.
 * @return Zero or -ENOEXEC for malformed/unsupported ELF; no locks or allocations.
 */
int module_elf_validate(const void *image,size_t size,struct module_elf *out)
{
    if(!image || !out || size<sizeof(struct module_elf_header)) return -ENOEXEC;
    struct module_elf view={.image=image,.size=size};
    module_copy(&view.header,image,sizeof(view.header));
    const struct module_elf_header *h=&view.header;
    if(h->ident[0]!=0x7f || h->ident[1]!='E' || h->ident[2]!='L' || h->ident[3]!='F' ||
       h->ident[4]!=2 || h->ident[5]!=1 || h->ident[6]!=1 ||
       (h->ident[7]!=0 && h->ident[7]!=3) || h->ident[8] || h->type!=1 || h->machine!=62 ||
       h->version!=1 || h->flags || h->entry || h->phnum || h->phoff ||
       h->ehsize!=sizeof(*h) || h->shentsize!=sizeof(struct module_elf_section) ||
       !h->shnum || h->shstrndx==0 || h->shstrndx>=h->shnum ||
       h->shoff<sizeof(*h) || !image_range(size,h->shoff,(uint64_t)h->shnum*h->shentsize))
        return -ENOEXEC;
    if(module_elf_get_section(&view,h->shstrndx,&view.section_names) ||
       view.section_names.type!=3 || !view.section_names.size ||
       !image_range(size,view.section_names.offset,view.section_names.size)) return -ENOEXEC;
    for(unsigned int i=0;i<h->shnum;++i) {
        struct module_elf_section section;
        if(module_elf_get_section(&view,i,&section)) return -ENOEXEC;
        if(!i) {
            struct module_elf_section empty={0};
            const uint8_t *left=(const uint8_t *)&section,*right=(const uint8_t *)&empty;
            for(size_t n=0;n<sizeof(section);++n) if(left[n]!=right[n]) return -ENOEXEC;
            continue;
        }
        if(!module_elf_section_name(&view,i) ||
           (section.type!=8 && !image_range(size,section.offset,section.size)) ||
           (section.alignment && (section.alignment&(section.alignment-1))) ||
           (section.flags&(0x400u|0x800u)) || /* TLS and compressed sections */
           ((section.flags&(MODULE_ELF_WRITE|MODULE_ELF_EXEC))==(MODULE_ELF_WRITE|MODULE_ELF_EXEC)) ||
           section.type==9) return -ENOEXEC; /* x86-64 requires RELA, not REL */
        if(section.type==2) {
            if(view.symbol_section || section.entry_size!=sizeof(struct module_elf_symbol) ||
               !section.size || section.size%sizeof(struct module_elf_symbol) ||
               !section.link || section.link>=h->shnum) return -ENOEXEC;
            view.symbol_section=i; view.symbols=section;
            view.symbol_count=section.size/sizeof(struct module_elf_symbol);
        }
    }
    if(!view.symbol_section || view.symbols.info>view.symbol_count ||
       module_elf_get_section(&view,view.symbols.link,&view.strings) ||
       view.strings.type!=3 || !view.strings.size || view.image[view.strings.offset]) return -ENOEXEC;
    if(validate_symbols(&view)) return -ENOEXEC;
    for(unsigned int i=1;i<h->shnum;++i) {
        struct module_elf_section section;
        if(module_elf_get_section(&view,i,&section)) return -ENOEXEC;
        if(section.type==4 && validate_relocations(&view,&section)) return -ENOEXEC;
    }
    *out=view;
    return 0;
}

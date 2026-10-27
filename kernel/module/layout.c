/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>

/** @brief Release every section and metadata allocation of a quiesced private layout.
 * @param layout Owned or partially initialized layout; NULL is accepted and success zeroes it.
 * Task context; caller has stopped execution and external users before release.
 */
void module_layout_destroy(struct module_layout *layout)
{
    if(!layout) return;
    if(layout->sections) for(size_t i=0;i<layout->section_count;++i)
        module_memory_free(layout->sections[i].memory);
    kernel_free(layout->sections); kernel_free(layout->symbols);
    module_zero(layout,sizeof(*layout));
}
/** @brief Allocate independent RW/NX section regions and materialize NOBITS as zeroes.
 * @param view Validated immutable ELF retained during construction.
 * @param out Empty caller-owned layout.
 * @return Zero or negative errno; errors release the entire partial transaction.
 * Sleepable task context; mapping metadata stays private and no module code runs.
 */
int module_layout_create(const struct module_elf *view,struct module_layout *out)
{
    if(!view || !out || out->sections || out->symbols) return -EINVAL;
    out->section_count=view->header.shnum; out->symbol_count=view->symbol_count;
    if(out->symbol_count>SIZE_MAX/sizeof(*out->symbols)) goto malformed;
    out->sections=kernel_malloc(out->section_count*sizeof(*out->sections));
    if(!out->sections) goto nomemory;
    module_zero(out->sections,out->section_count*sizeof(*out->sections));
    out->symbols=kernel_malloc(out->symbol_count*sizeof(*out->symbols));
    if(!out->symbols) goto nomemory;
    module_zero(out->symbols,out->symbol_count*sizeof(*out->symbols));
    for(unsigned int i=1;i<view->header.shnum;++i) {
        struct module_elf_section section;
        if(module_elf_get_section(view,i,&section)) goto malformed;
        if(!(section.flags&MODULE_ELF_ALLOC)) continue;
        out->sections[i].size=section.size; out->sections[i].flags=section.flags;
        if(!section.size) continue;
        if(section.size>SIZE_MAX) goto malformed;
        int rc=module_memory_alloc((size_t)section.size,(size_t)section.alignment,
                                    &out->sections[i].memory);
        if(rc) { module_layout_destroy(out); return rc; }
        void *destination=(void *)out->sections[i].memory->address;
        if(section.type==8) module_zero(destination,(size_t)section.size);
        else module_copy(destination,view->image+section.offset,(size_t)section.size);
    }
    return 0;
nomemory:
    module_layout_destroy(out); return -ENOMEM;
malformed:
    module_layout_destroy(out); return -ENOEXEC;
}
/** @brief Resolve a symbol without allowing references to discarded nonalloc sections.
 * @param view Immutable checked ELF.
 * @param layout Private allocated layout.
 * @param index Symbol-table index.
 * @param lookup Optional external resolver.
 * @param context Resolver-owned context.
 * @return Zero or negative errno; resolved addresses remain owned by their providers.
 */
static int resolve_symbol(const struct module_elf *view,struct module_layout *layout,
                           size_t index,module_symbol_lookup_fn lookup,void *context)
{
    struct module_elf_symbol symbol;
    if(module_elf_get_symbol(view,index,&symbol)) return -ENOEXEC;
    if(symbol.section==MODULE_ELF_UNDEF) {
        if(!index) return 0;
        uint64_t address=0;
        const char *name=module_elf_symbol_name(view,&symbol);
        int rc=lookup ? lookup(name,&address,context) : -ENOENT;
        if(rc && (symbol.info>>4)!=2) return rc;
        layout->symbols[index]=rc ? 0 : address;
    } else if(symbol.section==MODULE_ELF_ABS) layout->symbols[index]=symbol.value;
    else {
        struct module_layout_section *section=&layout->sections[symbol.section];
        if(!section->memory) return 0; /* Debug-only definitions are not published. */
        if(symbol.value>UINT64_MAX-section->memory->address) return -ENOEXEC;
        layout->symbols[index]=section->memory->address+symbol.value;
    }
    return 0;
}
/** @brief Resolve dependencies then relocate each allocated target section.
 * @param view Validated ELF matching layout.
 * @param layout Private RW/NX regions; caller keeps ownership on error.
 * @param lookup Optional resolver; may acquire provider references in context.
 * @param context Borrowed resolver state; no global lock held across callbacks.
 * @return Zero or negative errno; failure must be followed by layout destruction.
 */
int module_layout_relocate(const struct module_elf *view,struct module_layout *layout,
                            module_symbol_lookup_fn lookup,void *context)
{
    if(!view || !layout || layout->section_count!=view->header.shnum ||
       layout->symbol_count!=view->symbol_count) return -EINVAL;
    for(size_t i=0;i<view->symbol_count;++i) {
        int rc=resolve_symbol(view,layout,i,lookup,context);
        if(rc) return rc;
    }
    for(unsigned int i=1;i<view->header.shnum;++i) {
        struct module_elf_section section;
        if(module_elf_get_section(view,i,&section)) return -ENOEXEC;
        if(section.type!=4) continue;
        struct module_layout_section *target=&layout->sections[section.info];
        if(!(target->flags&MODULE_ELF_ALLOC)) continue;
        for(uint64_t offset=0;offset<section.size;offset+=sizeof(struct module_elf_rela)) {
            struct module_elf_rela rela;
            module_copy(&rela,view->image+section.offset+offset,sizeof(rela));
            if(!target->memory && module_relocation_width((unsigned int)rela.info)) return -ENOEXEC;
            uint64_t address=target->memory ? target->memory->address+rela.offset : 0;
            struct module_elf_symbol symbol;
            if(module_elf_get_symbol(view,(size_t)(rela.info>>32),&symbol)) return -ENOEXEC;
            if(symbol.section!=MODULE_ELF_UNDEF && symbol.section!=MODULE_ELF_ABS &&
               !layout->sections[symbol.section].memory && (unsigned int)rela.info) return -ENOEXEC;
            int rc=module_apply_relocation((void *)(uintptr_t)address,
                    (size_t)(target->size-rela.offset),(unsigned int)rela.info,
                    layout->symbols[rela.info>>32],rela.addend,address);
            if(rc) return rc;
        }
    }
    return 0;
}
/** @brief Find a defined allocated symbol after relocation.
 * @param view Immutable ELF table.
 * @param layout Private resolved layout.
 * @param name Terminated symbol name.
 * @return Address or zero; does not acquire an external reference.
 */
uint64_t module_layout_find_symbol(const struct module_elf *view,
                                   const struct module_layout *layout,const char *name)
{
    if(!view || !layout || !name) return 0;
    for(size_t i=1;i<view->symbol_count;++i) {
        struct module_elf_symbol symbol;
        if(module_elf_get_symbol(view,i,&symbol)) return 0;
        const char *candidate=module_elf_symbol_name(view,&symbol);
        if(candidate && symbol.section!=MODULE_ELF_UNDEF && module_string_equal(name,candidate))
            return layout->symbols[i];
    }
    return 0;
}
/** @brief Apply final W^X protection to every allocated section before publication.
 * @param layout Exclusively owned writable layout.
 * @return Zero or negative errno; partial protection failure still requires destruction.
 * Task context; no execution until all mappings and aliases have synchronized.
 */
int module_layout_seal(struct module_layout *layout)
{
    if(!layout || !layout->sections) return -EINVAL;
    for(size_t i=1;i<layout->section_count;++i) {
        struct module_layout_section *section=&layout->sections[i];
        if(!section->memory) continue;
        unsigned int permissions=0;
        if(section->flags&MODULE_ELF_WRITE) permissions|=MODULE_MEMORY_WRITE;
        if(section->flags&MODULE_ELF_EXEC) permissions|=MODULE_MEMORY_EXEC;
        int rc=module_memory_protect(section->memory,permissions);
        if(rc) return rc;
    }
    return 0;
}
/** @brief Check a pointer against real allocated bytes, excluding rounded page padding.
 * @param layout Stable loaded layout owned by caller.
 * @param address Candidate runtime address.
 * @param size Complete byte interval required.
 * @param required Required ELF section flags, such as WRITE or EXEC.
 * @return True when contained in one suitable section; no locks/references acquired.
 */
bool module_layout_contains(const struct module_layout *layout,uint64_t address,size_t size,uint64_t required)
{
    if(!layout) return false;
    for(size_t i=1;i<layout->section_count;++i) {
        const struct module_layout_section *s=&layout->sections[i];
        if(!s->memory || (s->flags&required)!=required || address<s->memory->address) continue;
        uint64_t offset=address-s->memory->address;
        if(offset<=s->size && size<=s->size-offset) return true;
    }
    return false;
}
/** @brief Locate a terminated string without scanning outside its real mapped section.
 * @param layout Stable loaded layout.
 * @param address Candidate module string pointer.
 * @return Borrowed pointer or NULL; no reference or lock acquired.
 */
const char *module_layout_string(const struct module_layout *layout,const char *address)
{
    if(!layout || !address) return NULL;
    uintptr_t value=(uintptr_t)address;
    for(size_t i=1;i<layout->section_count;++i) {
        const struct module_layout_section *s=&layout->sections[i];
        if(!s->memory || value<s->memory->address) continue;
        uint64_t offset=value-s->memory->address;
        if(offset>=s->size) continue;
        for(uint64_t n=offset;n<s->size;++n) if(!address[n-offset]) return address;
        return NULL;
    }
    return NULL;
}
/** @brief Locate mapped storage by its validated ELF section name.
 * @param view Immutable ELF metadata.
 * @param layout Matching private/loaded layout.
 * @param name Terminated section name.
 * @param size Required size result, initialized to zero when absent.
 * @return Borrowed section pointer or NULL, without locks or ownership transfer.
 */
void *module_layout_named(const struct module_elf *view,const struct module_layout *layout,
                          const char *name,size_t *size)
{
    *size=0;
    for(unsigned i=1;i<view->header.shnum;++i) {
        const char *candidate=module_elf_section_name(view,i);
        if(candidate && module_string_equal(candidate,name)) {
            const struct module_layout_section *s=&layout->sections[i];
            if(!s->memory) return NULL;
            *size=(size_t)s->size; return (void *)s->memory->address;
        }
    }
    return NULL;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>

extern const struct module_export_symbol __start___ksymtab[] __attribute__((weak));
extern const struct module_export_symbol __stop___ksymtab[] __attribute__((weak));
extern const uint32_t __start___kcrctab[] __attribute__((weak));
extern const uint32_t __stop___kcrctab[] __attribute__((weak));
extern const struct module_export_symbol __start___ksymtab_gpl[] __attribute__((weak));
extern const struct module_export_symbol __stop___ksymtab_gpl[] __attribute__((weak));
extern const uint32_t __start___kcrctab_gpl[] __attribute__((weak));
extern const uint32_t __stop___kcrctab_gpl[] __attribute__((weak));

/** @brief Find a named entry in an already validated export table.
 * @param table Stable table owned by built-in image or pinned module.
 * @param name Terminated lookup name.
 * @return Index, or SIZE_MAX; no locking or references acquired internally.
 */
static size_t table_find(const struct module_export_table *table,const char *name)
{
    for(size_t i=0;i<table->count;++i)
        if(module_string_equal(table->symbols[i].name,name)) return i;
    return SIZE_MAX;
}
/** @brief Obtain one immutable linker-built kernel export table.
 * @param gpl Nonzero for GPL-only exports.
 * @return Borrowed table; malformed/missing linker boundaries produce an empty table.
 */
static struct module_export_table builtin_table(unsigned int gpl)
{
    const struct module_export_symbol *start=gpl ? __start___ksymtab_gpl : __start___ksymtab;
    const struct module_export_symbol *end=gpl ? __stop___ksymtab_gpl : __stop___ksymtab;
    const uint32_t *crcs=gpl ? __start___kcrctab_gpl : __start___kcrctab;
    const uint32_t *crc_end=gpl ? __stop___kcrctab_gpl : __stop___kcrctab;
    struct module_export_table table={.symbols=start,.crcs=crcs,.gpl_only=gpl!=0};
    if(start && end && crcs && crc_end && (uintptr_t)end>=(uintptr_t)start &&
       (uintptr_t)crc_end>=(uintptr_t)crcs) {
        size_t bytes=(uintptr_t)end-(uintptr_t)start;
        if(!(bytes%sizeof(*start)) && bytes/sizeof(*start)==((uintptr_t)crc_end-(uintptr_t)crcs)/sizeof(*crcs))
            table.count=bytes/sizeof(*start);
    }
    return table;
}
/** @brief Recognize the explicit GPL-compatible license strings accepted by Linux modpost.
 * @param license Validated terminated license metadata.
 * @return True for an explicitly compatible license; Apache-only is not mislabelled GPL.
 */
static bool gpl_compatible(const char *license)
{
    static const char *const accepted[]={"GPL","GPL v2","GPL and additional rights",
        "Dual BSD/GPL","Dual MIT/GPL","Dual MPL/GPL"};
    for(size_t i=0;i<sizeof(accepted)/sizeof(*accepted);++i)
        if(module_string_equal(license,accepted[i])) return true;
    return false;
}
/** @brief Read the unique expected CRC of one native import.
 * @param runtime Stable importing runtime.
 * @param name Terminated symbol name.
 * @param crc Required CRC output.
 * @return Zero or -ENOEXEC for missing/duplicate version metadata.
 */
static int expected_crc(const struct module_runtime *runtime,const char *name,uint32_t *crc)
{
    bool found=false;
    for(size_t i=0;i<runtime->version_count;++i)
        if(module_string_equal(runtime->versions[i].name,name)) {
            if(found) return -ENOEXEC;
            *crc=(uint32_t)runtime->versions[i].crc; found=true;
        }
    return found ? 0 : -ENOEXEC;
}
/** @brief Validate bounded modversion records before any external address is resolved.
 * @param view Checked ELF matching runtime layout.
 * @param runtime Private runtime receiving borrowed mapped table pointers.
 * @return Zero or -ENOEXEC; no allocations or locks.
 */
int module_versions_prepare(const struct module_elf *view,struct module_runtime *runtime)
{
    size_t bytes=0;
    runtime->versions=module_layout_named(view,&runtime->layout,"__versions",&bytes);
    if(bytes%sizeof(*runtime->versions) || (bytes && (uintptr_t)runtime->versions%_Alignof(struct modversion_info)))
        return -ENOEXEC;
    runtime->version_count=bytes/sizeof(*runtime->versions);
    for(size_t i=0;i<runtime->version_count;++i) {
        const struct modversion_info *version=&runtime->versions[i];
        if(version->crc>UINT32_MAX || !version->name[0]) return -ENOEXEC;
        size_t n=0; while(n<MODULE_NAME_LEN && version->name[n]) ++n;
        if(n==MODULE_NAME_LEN) return -ENOEXEC;
        for(size_t j=0;j<i;++j)
            if(module_string_equal(version->name,runtime->versions[j].name)) return -ENOEXEC;
    }
    /* modpost adds this version without a code relocation. It still binds
     * the descriptor/parameter/export layouts even for import-free modules. */
    uint32_t wanted=0;
    if(expected_crc(runtime,"module_layout",&wanted)) return -ENOEXEC;
    struct module_export_table core=builtin_table(0);
    size_t index=table_find(&core,"module_layout");
    return index!=SIZE_MAX && core.crcs[index]==wanted ? 0 : -ENOEXEC;
}
/** @brief Compare an export's CRC, license and namespace against importer metadata.
 * @param runtime Importing runtime with validated license/version table.
 * @param table Stable provider table.
 * @param index Valid index in provider table.
 * @param expected Expected symbol CRC.
 * @return Zero or a negative errno; does not acquire references.
 */
static int check_import(const struct module_runtime *runtime,const struct module_export_table *table,
                         size_t index,uint32_t expected)
{
    if(table->crcs[index]!=expected) return -ENOEXEC;
    if(table->gpl_only && !gpl_compatible(runtime->license)) return -EPERM;
    if(!module_imports_namespace(runtime,table->symbols[index].namespace)) return -EINVAL;
    return 0;
}
/** @brief Resolve one import and pin its provider, never allocating under the registry lock.
 * @param name Validated terminated ELF symbol name.
 * @param address Required resolved address.
 * @param context Private importing module runtime.
 * @return Zero or negative errno; -ENOENT means no actual export exists. Missing
 * version metadata for an existing export is -ENOEXEC, including weak imports.
 * Provider pins become runtime-owned dependencies only after all checks pass.
 */
int module_resolve_symbol(const char *name,uint64_t *address,void *context)
{
    struct module_runtime *runtime=context;
    uint32_t expected=0;
    int version_rc=expected_crc(runtime,name,&expected),rc;
    for(unsigned t=0;t<2;++t) {
        struct module_export_table table=builtin_table(t);
        size_t i=table_find(&table,name);
        if(i!=SIZE_MAX) {
            rc=version_rc ? version_rc : check_import(runtime,&table,i,expected);
            if(!rc) *address=table.symbols[i].value;
            return rc;
        }
    }
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    struct module *provider=NULL;
    rc=-ENOENT;
    for(struct module_runtime *r=module_list;r && !provider;r=r->next) {
        if(!r->exports_ready) continue;
        for(unsigned t=0;t<2;++t) {
            size_t i=table_find(&r->exports[t],name);
            if(i==SIZE_MAX) continue;
            rc=version_rc ? version_rc : check_import(runtime,&r->exports[t],i,expected);
            if(!rc && r==runtime) rc=-ELOOP;
            if(!rc && r->state!=MODULE_LIVE) rc=-EBUSY;
            if(!rc && r->users==UINT64_MAX) rc=-EOVERFLOW;
            if(!rc) {
                provider=r->owner; ++r->users;
                *address=r->exports[t].symbols[i].value;
            }
            break;
        }
        if(rc!=-ENOENT) break;
    }
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
    if(rc) return rc;
    struct module_dependency *dependency=kernel_malloc(sizeof(*dependency));
    if(!dependency) { module_put(provider); return -ENOMEM; }
    kernel_spin_lock_irqsave(&module_table_lock,&flags);
    bool exists=false;
    for(struct module_dependency *d=runtime->dependencies;d;d=d->next)
        if(d->owner==provider) { exists=true; break; }
    if(!exists) {
        *dependency=(struct module_dependency){.next=runtime->dependencies,.owner=provider};
        runtime->dependencies=dependency;
    }
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
    if(exists) { kernel_free(dependency); module_put(provider); }
    return 0;
}
/** @brief Release all provider pins once the importing runtime is unlisted and quiesced.
 * @param runtime Exclusively cleanup-owned runtime, no registry lock held.
 */
void module_dependencies_release(struct module_runtime *runtime)
{
    while(runtime->dependencies) {
        struct module_dependency *d=runtime->dependencies;
        runtime->dependencies=d->next; module_put(d->owner); kernel_free(d);
    }
}
/** @brief Validate native relocated export arrays before publishing their names.
 * @param view Checked ELF matching runtime layout.
 * @param runtime COMING runtime whose layout is still private.
 * @return Zero or negative errno; export reservation is atomic under registry lock.
 */
int module_symbols_prepare(const struct module_elf *view,struct module_runtime *runtime)
{
    static const char *const symbols[]={"__ksymtab","__ksymtab_gpl"};
    static const char *const crcs[]={"__kcrctab","__kcrctab_gpl"};
    for(unsigned t=0;t<2;++t) {
        size_t bytes=0,crc_bytes=0;
        struct module_export_table *table=&runtime->exports[t];
        table->symbols=module_layout_named(view,&runtime->layout,symbols[t],&bytes);
        table->crcs=module_layout_named(view,&runtime->layout,crcs[t],&crc_bytes);
        table->gpl_only=t!=0;
        if(bytes%sizeof(*table->symbols) || bytes/sizeof(*table->symbols)!=crc_bytes/sizeof(*table->crcs) ||
           crc_bytes%sizeof(*table->crcs) || (bytes && (uintptr_t)table->symbols%_Alignof(struct module_export_symbol)) ||
           (crc_bytes && (uintptr_t)table->crcs%_Alignof(uint32_t))) return -ENOEXEC;
        table->count=bytes/sizeof(*table->symbols);
        for(size_t i=0;i<table->count;++i) {
            const struct module_export_symbol *symbol=&table->symbols[i];
            if(!symbol->value || !module_layout_string(&runtime->layout,symbol->name) || !*symbol->name ||
               !module_layout_string(&runtime->layout,symbol->namespace) ||
               !module_layout_contains(&runtime->layout,symbol->value,1,0)) return -ENOEXEC;
            for(unsigned previous=0;previous<=t;++previous)
                for(size_t j=0;j<(previous==t ? i : runtime->exports[previous].count);++j)
                    if(module_string_equal(symbol->name,runtime->exports[previous].symbols[j].name)) return -ENOEXEC;
        }
    }
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    int rc=0;
    for(unsigned t=0;t<2 && !rc;++t) for(size_t i=0;i<runtime->exports[t].count && !rc;++i) {
        const char *name=runtime->exports[t].symbols[i].name;
        for(unsigned k=0;k<2;++k) {
            struct module_export_table builtin=builtin_table(k);
            if(table_find(&builtin,name)!=SIZE_MAX) rc=-EEXIST;
        }
        for(struct module_runtime *r=module_list;r && !rc;r=r->next) {
            if(r==runtime || !r->exports_ready) continue;
            for(unsigned k=0;k<2;++k)
                if(table_find(&r->exports[k],name)!=SIZE_MAX) rc=-EEXIST;
        }
    }
    if(!rc) runtime->exports_ready=true;
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
    return rc;
}

/* The original modpost adds a version reference to module_layout. Its type
 * deliberately covers the native public module and registration layouts. */
#include <reliefnt/moduleparam.h>
#include <reliefnt/export.h>
/** @brief Supply a versioned type anchor consumed by original modpost.
 * @param owner Unused module-layout type input; no ownership transfer.
 * @param parameter Unused parameter-layout type input; may be NULL.
 * @param symbol Unused export-layout type input; may be NULL.
 * This anchor performs no runtime operation and acquires no lock/reference.
 */
void module_layout(struct module *owner,const struct module_parameter *parameter,
                    const struct module_export_symbol *symbol);
/** @brief Define the native module ABI's exported type-CRC anchor.
 * @param owner Unused module descriptor; may be NULL.
 * @param parameter Unused parameter descriptor; may be NULL.
 * @param symbol Unused symbol descriptor; may be NULL.
 * No ownership, locking, allocation or callback effects.
 */
void module_layout(struct module *owner,const struct module_parameter *parameter,
                    const struct module_export_symbol *symbol)
{
    (void)owner; (void)parameter; (void)symbol;
}
EXPORT_SYMBOL(module_layout);

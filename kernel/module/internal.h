/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MODULE_INTERNAL_H
#define RELIEFNT_MODULE_INTERNAL_H
#include <reliefnt/module.h>
#include <reliefnt/module_memory.h>
#include "../params/internal.h"
#include <linux/errno.h>

#define MODULE_ELF_ALLOC 2u
#define MODULE_ELF_WRITE 1u
#define MODULE_ELF_EXEC 4u
#define MODULE_ELF_UNDEF 0u
#define MODULE_ELF_ABS 0xfff1u

struct module_elf_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct module_elf_section {
    uint32_t name, type;
    uint64_t flags, address, offset, size;
    uint32_t link, info;
    uint64_t alignment, entry_size;
};
struct module_elf_symbol {
    uint32_t name;
    uint8_t info, other;
    uint16_t section;
    uint64_t value, size;
};
struct module_elf_rela { uint64_t offset, info; int64_t addend; };
_Static_assert(sizeof(struct module_elf_header)==64, "ELF64 header");
_Static_assert(sizeof(struct module_elf_section)==64, "ELF64 section");
_Static_assert(sizeof(struct module_elf_symbol)==24, "ELF64 symbol");
_Static_assert(sizeof(struct module_elf_rela)==24, "ELF64 RELA");

struct module_elf {
    const uint8_t *image;
    size_t size;
    struct module_elf_header header;
    struct module_elf_section symbols, strings, section_names;
    uint32_t symbol_section;
    size_t symbol_count;
};
struct module_layout_section {
    struct module_memory *memory;
    uint64_t size, flags;
};
struct module_layout {
    struct module_layout_section *sections;
    uint64_t *symbols;
    size_t section_count, symbol_count;
};
typedef int (*module_symbol_lookup_fn)(const char *name, uint64_t *address, void *context);

/** @brief Copy a bounded nonoverlapping byte range, including unaligned ELF fields.
 * @param target Writable destination with room for length bytes.
 * @param source Readable nonoverlapping source with length bytes.
 * @param length Number of bytes; no ownership transfer or locks.
 */
static inline void module_copy(void *target, const void *source, size_t length)
{
    uint8_t *to=target; const uint8_t *from=source;
    for(size_t i=0;i<length;++i) to[i]=from[i];
}
/** @brief Zero caller-owned metadata without alignment assumptions.
 * @param target Writable destination.
 * @param length Number of bytes; no ownership transfer or locks.
 */
static inline void module_zero(void *target,size_t length)
{
    uint8_t *to=target; for(size_t i=0;i<length;++i) to[i]=0;
}
/** @brief Compare two validated NUL-terminated kernel strings.
 * @param left Readable first string.
 * @param right Readable second string.
 * @return True for equality; no ownership or locking effects.
 */
static inline bool module_string_equal(const char *left,const char *right)
{
    while(*left && *left==*right) { ++left; ++right; }
    return *left==*right;
}

/** @brief Validate a stable kernel-owned ET_REL image before allocation or execution.
 * @param image Immutable bytes, possibly unaligned; retained by caller.
 * @param size Readable byte length.
 * @param out Required borrowed view valid until image changes or is freed.
 * @return Zero or -ENOEXEC; no allocations, locks or ownership transfer.
 */
int module_elf_validate(const void *image,size_t size,struct module_elf *out);
/** @brief Read a validated section header without unaligned struct access.
 * @param view Stable borrowed ELF view.
 * @param index Section index.
 * @param out Required writable section copy.
 * @return Zero or -ENOEXEC; no ownership transfer or locks.
 */
int module_elf_get_section(const struct module_elf *view,unsigned int index,struct module_elf_section *out);
/** @brief Read one validated symbol into aligned caller storage.
 * @param view Stable ELF view.
 * @param index Symbol-table index.
 * @param out Required symbol copy.
 * @return Zero or -ENOEXEC; no ownership transfer or locks.
 */
int module_elf_get_symbol(const struct module_elf *view,size_t index,struct module_elf_symbol *out);
/** @brief Locate a bounded NUL-terminated section-name string.
 * @param view Stable validated ELF view.
 * @param index Section index.
 * @return Borrowed name or NULL for invalid input; no locks.
 */
const char *module_elf_section_name(const struct module_elf *view,unsigned int index);
/** @brief Locate a bounded NUL-terminated symbol-name string.
 * @param view Stable validated ELF view.
 * @param symbol Validated symbol copy.
 * @return Borrowed name or NULL for invalid input; no locks.
 */
const char *module_elf_symbol_name(const struct module_elf *view,const struct module_elf_symbol *symbol);
/** @brief Compute the checked byte width of a supported x86-64 relocation.
 * @param type ELF64 x86-64 relocation selector.
 * @return Width in bytes (zero for NONE), or -ENOEXEC for unsupported types.
 */
int module_relocation_width(unsigned int type);
/** @brief Apply one x86-64 RELA only after all arithmetic and bounds checks succeed.
 * @param target Writable relocation target; no write on error.
 * @param available Remaining target-section bytes.
 * @param type Supported x86-64 RELA type.
 * @param symbol Resolved absolute symbol address.
 * @param addend Signed explicit ELF addend.
 * @param place Runtime address of target.
 * @return Zero or -ENOEXEC for overflow/type/bounds; loading task owns writable mapping.
 */
int module_apply_relocation(void *target,size_t available,unsigned int type,uint64_t symbol,int64_t addend,uint64_t place);
/** @brief Allocate and populate separate RW/NX mappings for all SHF_ALLOC sections.
 * @param view Stable validated ELF view retained by caller.
 * @param out Zeroed caller-owned layout; owns every allocation on success.
 * @return Zero or negative errno; failure completely destroys the partial layout.
 */
int module_layout_create(const struct module_elf *view,struct module_layout *out);
/** @brief Resolve symbols and apply validated RELA in an exclusively owned writable layout.
 * @param view Stable validated ELF view matching layout.
 * @param layout Owned layout; remains owned by caller on every return.
 * @param lookup Optional external symbol resolver; NULL rejects unresolved strong symbols.
 * @param context Borrowed resolver context; no global module lock may span this call.
 * @return Zero or negative errno; caller destroys partial relocation on failure.
 */
int module_layout_relocate(const struct module_elf *view,struct module_layout *layout,module_symbol_lookup_fn lookup,void *context);
/** @brief Look up a defined symbol in an already relocated private layout.
 * @param view Stable ELF view matching layout.
 * @param layout Resolved caller-owned layout.
 * @param name NUL-terminated symbol name.
 * @return Borrowed runtime address or zero; no reference acquired and no locks.
 */
uint64_t module_layout_find_symbol(const struct module_elf *view,const struct module_layout *layout,const char *name);
/** @brief Seal each section as RX, RO/NX or RW/NX before publishing module code.
 * @param layout Exclusively owned relocated layout; no execution before success.
 * @return Zero or negative errno; all backing remains owned even on partial failure.
 */
int module_layout_seal(struct module_layout *layout);
/** @brief Destroy an unpublished or fully quiesced layout, including partial creation.
 * @param layout Caller-owned layout or NULL; zeroed after release. Task context.
 */
void module_layout_destroy(struct module_layout *layout);
struct module_export_symbol {
    uintptr_t value;
    const char *name, *namespace;
};
struct module_export_table {
    const struct module_export_symbol *symbols;
    const uint32_t *crcs;
    size_t count;
    bool gpl_only;
};
struct module_dependency { struct module_dependency *next; struct module *owner; };
struct module_resource {
    struct module_resource *next;
    int (*quiesce)(void *);
    void (*release)(void *);
    void *context;
    bool stopped;
};
/* Separate from module users and layout: open sysfs files may keep this small
 * token after the actual provider has been stopped and its code unmapped. */
struct module_identity {uint64_t references;bool alive;};
struct module_runtime {
    struct module_runtime *next;
    struct module_identity *identity;
    char name[MODULE_NAME_LEN];
    struct module *owner;
    enum module_state state;
    uint64_t users, executing, memory_size;
    bool exports_ready, init_succeeded, exit_called, cleanup_running;
    struct module_layout layout;
    struct module_export_table exports[2];
    const struct modversion_info *versions;
    size_t version_count;
    struct module_dependency *dependencies;
    struct module_resource *resources;
    char *modinfo;
    struct parameter_values parameters;
    size_t modinfo_size;
    const char *license;
};
#include <reliefnt/lock.h>
extern struct kernel_spinlock module_table_lock;
extern struct module_runtime *module_list;

/** @brief Validate an address interval within an allocated section.
 * @param layout Stable allocated layout.
 * @param address Runtime byte address.
 * @param size Required readable byte count.
 * @param required Required ELF section flags; zero allows any allocated section.
 * @return True only for a fully contained interval; no reference acquisition or locks.
 */
bool module_layout_contains(const struct module_layout *layout,uint64_t address,size_t size,uint64_t required);
/** @brief Validate a complete string within allocated module storage.
 * @param layout Stable allocated layout.
 * @param address Candidate string pointer.
 * @return Borrowed terminated string or NULL; does not scan padding/outside mappings.
 */
const char *module_layout_string(const struct module_layout *layout,const char *address);
/** @brief Read a named allocated section by its ELF name.
 * @param view Stable ELF view matching layout.
 * @param layout Owned layout.
 * @param name Terminated section name.
 * @param size Required output length, zero on missing section.
 * @return Borrowed mapped storage or NULL; no locks or ownership transfer.
 */
void *module_layout_named(const struct module_elf *view,const struct module_layout *layout,const char *name,size_t *size);
/** @brief Copy and validate critical module metadata before reserving its name.
 * @param view Checked immutable ELF view.
 * @param runtime Private zeroed runtime; receives owned modinfo storage and name.
 * @return Zero or negative errno; caller frees modinfo on every return.
 */
int module_metadata_prepare(const struct module_elf *view,struct module_runtime *runtime);
/** @brief Find one metadata value in validated owned modinfo.
 * @param runtime Stable runtime with modinfo retained.
 * @param key Terminated record key.
 * @return Borrowed first matching value or NULL; no locks or allocation.
 */
const char *module_metadata(const struct module_runtime *runtime,const char *key);
/** @brief Check module namespace-import metadata.
 * @param runtime Stable runtime with modinfo retained.
 * @param namespace Terminated namespace; empty is always permitted.
 * @return True if explicitly imported or empty; no locks or ownership effects.
 */
bool module_imports_namespace(const struct module_runtime *runtime,const char *namespace);
/** @brief Resolve a versioned import and retain each providing module once.
 * @param name Terminated symbol name from validated ELF.
 * @param address Required resolved address output.
 * @param context Private importing module_runtime.
 * @return Zero or negative errno; acquired provider pins belong to runtime dependencies.
 */
int module_resolve_symbol(const char *name,uint64_t *address,void *context);
/** @brief Validate relocated exports and reserve their names against concurrent loads.
 * @param view Checked ELF matching runtime layout.
 * @param runtime Private loading runtime, already listed COMING.
 * @return Zero or negative errno; no execution or callbacks while the registry is locked.
 */
int module_symbols_prepare(const struct module_elf *view,struct module_runtime *runtime);
/** @brief Release dependency pins after a module is fully quiesced.
 * @param runtime Runtime exclusively owned by its cleanup path; no registry lock held.
 */
void module_dependencies_release(struct module_runtime *runtime);
/** @brief Validate and set parameters in an unpublished writable layout.
 * @param view Checked ELF matching runtime layout.
 * @param runtime Private loading runtime; owns replacement strings until unload.
 * @param arguments Borrowed terminated kernel parameters, NULL means empty.
 * @return Zero or negative errno; no module callbacks or global lock held.
 */
int module_parameters_prepare(const struct module_elf *view,struct module_runtime *runtime,const char *arguments);
/** @brief Validate the native symbol-version table before resolving imports.
 * @param view Checked ELF matching runtime layout.
 * @param runtime Private importing runtime receiving borrowed mapped version records.
 * @return Zero or negative errno; no allocation or ownership transfer.
 */
int module_versions_prepare(const struct module_elf *view,struct module_runtime *runtime);
#endif

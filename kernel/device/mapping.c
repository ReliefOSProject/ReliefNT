/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/device_mapping.h>
#include <reliefnt/export.h>
#include <reliefnt/heap.h>
#include <linux/errno.h>

struct device_mapping {
    struct module *owner;
    const struct device_mapping_operations *operations;
    void *context;
    struct device_mapping_info info;
    uint64_t references;
};
/** @brief Adopt a backend's initial reference only after provider/allocation admission.
 * @param owner LIVE module or NULL builtin.
 * @param operations Persistent release/physical callbacks.
 * @param context Backend state with one caller-owned backing reference.
 * @param info Borrowed immutable page extent and supported protection bits.
 * @param out Empty output receiving one mapping reference.
 * @return Zero or negative errno. Task/execution ownership required; errors
 * leave backing ownership unchanged, no locks span allocation or callbacks.
 */
int device_mapping_create(struct module *owner,const struct device_mapping_operations *operations,
                            void *context,const struct device_mapping_info *info,struct device_mapping **out)
{
    if(!out)return -EINVAL;*out=NULL;
    if(!operations || !operations->release || !operations->physical ||
       !info || !info->length || (info->length&4095u) || (info->max_prot&~7u))return -EINVAL;
    if(!try_module_get(owner))return -ENODEV;
    struct device_mapping *mapping=kernel_malloc(sizeof(*mapping));
    if(!mapping){module_put(owner);return -ENOMEM;}
    *mapping=(struct device_mapping){.owner=owner,.operations=operations,.context=context,.info=*info,.references=1};
    *out=mapping;return 0;
}
/** @brief Share the original backing lease through a core VMA reference.
 * @param mapping Borrowed mapping kept stable by one caller-owned reference.
 * @return Zero or EINVAL/EOVERFLOW with unchanged state on error. No backend
 * callback can reject a clone after PTE changes. Execution-owned task context;
 * no allocation or module/graph lock held.
 */
int device_mapping_get(struct device_mapping *mapping)
{
    if(!mapping)return -EINVAL;
    if(mapping->references==UINT64_MAX)return -EOVERFLOW;
    ++mapping->references;return 0;
}
/** @brief Release backing and final context before allowing provider-code retirement.
 * @param mapping Owned reference, nullable; consumed once.
 * Execution-owned task context without graph/module locks. Only the final
 * put destroys context and wrapper; all callbacks run with provider pinned.
 */
void device_mapping_put(struct device_mapping *mapping)
{
    if(!mapping)return;
    if(--mapping->references)return;
    struct module *owner=mapping->owner;
    mapping->operations->release(mapping->context);
    if(mapping->operations->destroy)mapping->operations->destroy(mapping->context);
    kernel_free(mapping);module_put(owner);
}
/** @brief Borrow immutable capabilities through a retained mapping reference.
 * @param mapping Borrowed retained object.
 * @return Borrowed info; execution ownership required, no allocation or locks.
 */
const struct device_mapping_info *device_mapping_info(const struct device_mapping *mapping)
{return &mapping->info;}
/** @brief Resolve only a page inside the backend's actual retained extent.
 * @param mapping Borrowed retained object.
 * @param offset Page-aligned byte position.
 * @return Aligned physical page or zero on bounds/translation failure. Task
 * execution ownership required; no reference transfer or registry locks.
 */
uint64_t device_mapping_physical(struct device_mapping *mapping,uint64_t offset)
{
    if(!mapping || (offset&4095u) || offset>=mapping->info.length)return 0;
    uint64_t physical=mapping->operations->physical(mapping->context,offset);
    return physical&4095u ? 0 : physical;
}
EXPORT_SYMBOL(device_mapping_create);
EXPORT_SYMBOL(device_mapping_get);
EXPORT_SYMBOL(device_mapping_put);
EXPORT_SYMBOL(device_mapping_physical);

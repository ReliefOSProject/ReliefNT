/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/device_mapping.h>
#include <reliefnt/export.h>
#include <reliefnt/heap.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <linux/errno.h>

/** @brief Hold a user DEVICE leaf's provider and a matching supervisor alias.
 * @param task Live task under execution ownership. @param address User byte.
 * @param physical Aligned admitted physical page. @param flags Admitted PTE flags.
 * @param out Empty owner populated on success. @return Zero or negative errno.
 * No user address or VMA pointer survives; release the alias before its backing.
 */
int device_user_page_get(struct task *task,uint64_t address,uint64_t physical,
                         uint64_t flags,struct device_user_page *out)
{
    if(!out)return -EINVAL;*out=(struct device_user_page){0};
    if(!task || !(flags&RELIEFNT_PAGE_DEVICE) || !(flags&RELIEFNT_PAGE_PRESENT) ||
       !(flags&RELIEFNT_PAGE_USER) || !physical || (physical&4095u) ||
       address_space_user_page_phys(sched_task_as(task),address)!=physical)
        return -EFAULT;
    struct task_vma *vma=NULL;
    for(uint32_t i=0;i<sched_task_vma_capacity(task);++i) {
        struct task_vma *candidate=sched_task_vma_at(task,i);
        if(candidate && candidate->used && (candidate->flags&TASK_VMA_FLAG_DEVICE) &&
           candidate->start<=address && address<candidate->end){vma=candidate;break;}
    }
    if(!vma)return -EFAULT;
    struct device_mapping *mapping=vma->device_mapping;
    uint64_t offset=0;
    if(mapping) {
        int rc=device_mapping_get(mapping);if(rc)return rc;
        uint64_t delta=(address&~4095ULL)-vma->start;
        if(vma->device_map_offset>UINT64_MAX-delta ||
           device_mapping_physical(mapping,vma->device_map_offset+delta)!=physical) {
            device_mapping_put(mapping);return -EFAULT;
        }
        offset=vma->device_map_offset+delta;
        rc=device_mapping_retain_range(mapping,offset,4096);
        if(rc){device_mapping_put(mapping);return rc;}
    } else if(vma->file_node.first_cluster!=STORAGE_DEV_KIND_FB0) return -EFAULT;

    const uint64_t mask=RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PAT;
    uint64_t cache=0;
    void *alias;
    bool temporary=false;
    if(paging_kernel_direct_cache_flags(physical,&cache)) {
        if(cache!=(flags&mask)) {
            device_mapping_release_range(mapping,offset,4096,true);
            device_mapping_put(mapping);return -EIO;
        }
        alias=paging_kernel_direct_map(physical);
    } else {
        alias=paging_ioremap_device_page(physical,flags&mask);temporary=true;
    }
    if(!alias){device_mapping_release_range(mapping,offset,4096,true);device_mapping_put(mapping);return -ENOMEM;}
    *out=(struct device_user_page){.mapping=mapping,.alias=alias,.offset=offset,.temporary=temporary};
    return 0;
}

/** @brief Retire one retained DEVICE alias and its original provider reference.
 * @param page Owner, cleared before final callbacks; NULL is accepted.
 * Execution-owned task context with no scheduler/graph/provider spinlock held.
 */
void device_user_page_put(struct device_user_page *page)
{
    if(!page)return;
    struct device_user_page old=*page;*page=(struct device_user_page){0};
    if(old.temporary && old.alias)(void)paging_iounmap(old.alias,4096);
    device_mapping_release_range(old.mapping,old.offset,4096,true);
    device_mapping_put(old.mapping);
}

struct device_policy_range {
    struct device_policy_range *next, *previous;
    struct device_policy_range *left, *right;
    uint64_t offset, end, owners;
    int height;
    struct paging_cache_claim claim;
};
struct device_mapping {
    struct module *owner;
    const struct device_mapping_operations *operations;
    void *context;
    struct device_mapping_info info;
    uint64_t references, retiring_references;
    struct device_mapping *parent;
    struct device_policy_range *policies, *policy_tail, *policy_cursor;
    struct device_policy_range *policy_tree;
    uint64_t policy_high_end;
    void (*drop_policies)(struct device_mapping *mapping);
};

#ifdef RELIEFNT_MM_TEST_STATS
uint64_t device_mapping_test_visits;
uint64_t device_mapping_test_tree_visits;
#define DEVICE_POLICY_VISIT() (++device_mapping_test_visits)
#define DEVICE_POLICY_TREE_VISIT() (++device_mapping_test_tree_visits)
#else
#define DEVICE_POLICY_VISIT() ((void)0)
#define DEVICE_POLICY_TREE_VISIT() ((void)0)
#endif

/** @brief Read a view-index subtree height.
 * @param node Borrowed subtree, nullable. @return Height or zero when empty.
 */
static int device_policy_height(const struct device_policy_range *node)
{ return node ? node->height : 0; }

/** @brief Refresh one view-index node after its children change.
 * @param node Nonnull node whose child heights are current.
 */
static void device_policy_update(struct device_policy_range *node)
{
    int left=device_policy_height(node->left),right=device_policy_height(node->right);
    node->height=1+(left>right ? left : right);
}

/** @brief Rotate one right-heavy view-index subtree.
 * @param root Node with a right child. @return New subtree root.
 */
static struct device_policy_range *device_policy_rotate_left(struct device_policy_range *root)
{
    struct device_policy_range *next=root->right;root->right=next->left;next->left=root;
    device_policy_update(root);device_policy_update(next);return next;
}

/** @brief Rotate one left-heavy view-index subtree.
 * @param root Node with a left child. @return New subtree root.
 */
static struct device_policy_range *device_policy_rotate_right(struct device_policy_range *root)
{
    struct device_policy_range *next=root->left;root->left=next->right;next->right=root;
    device_policy_update(root);device_policy_update(next);return next;
}

/** @brief Restore AVL heights after a view-index mutation.
 * @param root Nonnull mutated subtree. @return Balanced subtree root.
 */
static struct device_policy_range *device_policy_balance(struct device_policy_range *root)
{
    device_policy_update(root);
    int delta=device_policy_height(root->left)-device_policy_height(root->right);
    if(delta>1) {
        if(device_policy_height(root->left->left)<device_policy_height(root->left->right))
            root->left=device_policy_rotate_left(root->left);
        return device_policy_rotate_right(root);
    }
    if(delta< -1) {
        if(device_policy_height(root->right->right)<device_policy_height(root->right->left))
            root->right=device_policy_rotate_right(root->right);
        return device_policy_rotate_left(root);
    }
    return root;
}

/** @brief Index a distinct, already allocated view-policy node.
 * @param root Current subtree, nullable. @param node Owned unindexed node.
 * @return New subtree root. Execution ownership is held; no allocation or I/O.
 */
static struct device_policy_range *device_policy_insert(struct device_policy_range *root,
                                                         struct device_policy_range *node)
{
    if(!root) { node->left=node->right=NULL;node->height=1;return node; }
    DEVICE_POLICY_TREE_VISIT();
    if(node->offset<root->offset)root->left=device_policy_insert(root->left,node);
    else root->right=device_policy_insert(root->right,node);
    return device_policy_balance(root);
}

/** @brief Remove one exact view-policy node without moving its storage.
 * @param root Current subtree, nullable. @param node Indexed owned node.
 * @return New subtree root. Execution ownership is held; no allocation or I/O.
 */
static struct device_policy_range *device_policy_remove(struct device_policy_range *root,
                                                         struct device_policy_range *node)
{
    if(!root)return NULL;
    DEVICE_POLICY_TREE_VISIT();
    if(node->offset<root->offset)root->left=device_policy_remove(root->left,node);
    else if(node->offset>root->offset)root->right=device_policy_remove(root->right,node);
    else if(!root->left)return root->right;
    else if(!root->right)return root->left;
    else if(device_policy_height(root->left)>device_policy_height(root->right)) {
        root=device_policy_rotate_right(root);root->right=device_policy_remove(root->right,node);
    } else {
        root=device_policy_rotate_left(root);root->left=device_policy_remove(root->left,node);
    }
    return device_policy_balance(root);
}

/** @brief Find the first view policy intersecting or following an offset.
 * @param mapping Execution-owned mapping with sorted policy nodes.
 * @param offset Backing byte offset. @return Borrowed node or NULL.
 * Adjacent access uses a cursor; all other access uses the balanced index.
 * No allocation or I/O occurs, including on reverse and sparse lookups.
 */
static struct device_policy_range *device_policy_find(struct device_mapping *mapping,uint64_t offset)
{
    struct device_policy_range *p=mapping->policy_cursor;
    if(p && p->offset<=offset && p->end>offset) { DEVICE_POLICY_VISIT();return p; }
    if(p && p->next && p->next->offset<=offset && p->next->end>offset) {
        DEVICE_POLICY_VISIT();return p->next;
    }
    struct device_policy_range *result=NULL;
    p=mapping->policy_tree;
    while(p) {
        DEVICE_POLICY_VISIT();
        if(p->end>offset) { result=p;p=p->left; }
        else p=p->right;
    }
    return result;
}

/** @brief Retire every policy range after its provider callback has finished.
 * @param mapping Final owned mapping, whose PTEs and stages have retired.
 * Claims are unlinked before their node storage is freed; no provider callback
 * runs under the cache/MMIO lock.
 */
static void device_mapping_drop_policies(struct device_mapping *mapping)
{
    struct device_policy_range *policy=mapping->policies;
    while(policy) {
        struct device_policy_range *next=policy->next;
        DEVICE_POLICY_TREE_VISIT();
        paging_cache_claim_release(&policy->claim);
        kernel_free(policy);policy=next;
    }
    mapping->policies=NULL;mapping->policy_tail=NULL;mapping->policy_cursor=NULL;
    mapping->policy_tree=NULL;mapping->policy_high_end=0;
}
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
/** @brief Move one provider reference into a private per-mmap view.
 * @param mapping Owned provider reference, replaced only on success.
 * @return Zero or ENOMEM/EINVAL; no provider callback runs during allocation.
 */
int device_mapping_admit_view(struct device_mapping **mapping)
{
    if(!mapping || !*mapping)return -EINVAL;
    struct device_mapping *parent=*mapping;
    struct device_mapping *view=kernel_malloc(sizeof(*view));
    if(!view)return -ENOMEM;
    *view=(struct device_mapping){.operations=parent->operations,.context=parent->context,
        .info=parent->info,.references=1,.parent=parent};
    *mapping=view;return 0;
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
/** @brief Release backing and policy before allowing provider-code retirement.
 * @param mapping Owned reference, nullable; consumed once.
 * Execution-owned task context without graph/module locks. Only the final put
 * destroys context and wrapper; cache policy and code remain held during the
 * provider's release/destroy callbacks, and policy retires after those return.
 */
void device_mapping_put(struct device_mapping *mapping)
{
    if(!mapping)return;
    if(--mapping->references)return;
    struct module *owner=mapping->owner;
    struct device_mapping *parent=mapping->parent;
    if(parent)device_mapping_put(parent);
    else {
        mapping->operations->release(mapping->context);
        if(mapping->operations->destroy)mapping->operations->destroy(mapping->context);
    }
    if(mapping->drop_policies)mapping->drop_policies(mapping);
    kernel_free(mapping);if(!parent)module_put(owner);
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

/** @brief Retain the physical cache policy used by a DEVICE mapping page.
 * @param mapping Retained provider/backing. @param offset Aligned object offset.
 * @param physical Expected resolved page. @param flags Receives its cache bits.
 * @return Zero, EFAULT for a changed page, EIO for a conflicting policy or
 * ENOMEM. Admission views keep separate page owners; legacy provider objects
 * may coalesce adjacent high claims. No fixed page-count cap exists.
 */
int device_mapping_cache_page(struct device_mapping *mapping,uint64_t offset,
                              uint64_t physical,uint64_t *flags)
{
    if(!mapping || !flags || !physical || (offset&4095u) ||
       offset>=mapping->info.length || device_mapping_physical(mapping,offset)!=physical)
        return -EFAULT;
    /* The first pass reserves all owners; a later increasing PTE pass uses
     * the cursor instead of scanning the admission from its first page. */
    struct device_policy_range *before=NULL;
    if(offset<mapping->policy_high_end) {
        before=device_policy_find(mapping,offset);
        if(before && before->offset<=offset) {
            if(physical!=before->claim.start+offset-before->offset)return -EFAULT;
            mapping->policy_cursor=before;*flags=before->claim.flags;return 0;
        }
    }
    struct device_policy_range *policy=kernel_malloc(sizeof(*policy));
    if(!policy)return -ENOMEM;
    *policy=(struct device_policy_range){.offset=offset,.end=offset+4096,.owners=1};
    uint64_t cache;
    if(!paging_cache_claim_select(&policy->claim,physical,&cache)) {
        kernel_free(policy);return -EIO;
    }
    mapping->drop_policies=device_mapping_drop_policies;
    struct device_policy_range *tail=mapping->policy_tail;
    if(!mapping->parent && !before && tail && tail->end==offset &&
       paging_cache_claim_merge(&tail->claim,&policy->claim)) {
        tail->end=policy->end;kernel_free(policy);mapping->policy_cursor=tail;
    } else if(before) {
        policy->next=before;policy->previous=before->previous;
        if(before->previous)before->previous->next=policy;else mapping->policies=policy;
        before->previous=policy;mapping->policy_cursor=policy;
        mapping->policy_tree=device_policy_insert(mapping->policy_tree,policy);
    } else {
        policy->previous=tail;
        if(tail)tail->next=policy;else mapping->policies=policy;
        mapping->policy_tail=policy;mapping->policy_cursor=policy;
        mapping->policy_tree=device_policy_insert(mapping->policy_tree,policy);
    }
    if(offset+4096>mapping->policy_high_end)mapping->policy_high_end=offset+4096;
    *flags=cache;return 0;
}
/** @brief Validate alias ownership for every page in an admission view.
 * @param mapping Retained view. @param offset Aligned backing start.
 * @param length Aligned nonzero extent. @return Zero or negative errno; the
 * complete range is validated before any count changes. No allocation or I/O.
 */
int device_mapping_check_range(struct device_mapping *mapping,uint64_t offset,uint64_t length)
{
    if(!mapping)return 0;
    if(!mapping->parent)return 0;
    if(!length || (offset&4095u) || (length&4095u) || offset>mapping->info.length ||
       length>mapping->info.length-offset)return -EINVAL;
    uint64_t end=offset+length,expected=offset;
    struct device_policy_range *first=device_policy_find(mapping,offset);
    for(struct device_policy_range *p=first;p && p->offset<end;p=p->next) {
        DEVICE_POLICY_VISIT();
        if(p->offset!=expected || p->end!=p->offset+4096)return -EFAULT;
        if(!p->owners || p->owners==UINT64_MAX)return -EOVERFLOW;
        expected=p->end;
    }
    if(expected!=end)return -EFAULT;
    mapping->policy_cursor=first;return 0;
}
/** @brief Add one alias owner after validating the entire view range.
 * @param mapping Retained view. @param offset Aligned backing start.
 * @param length Aligned nonzero extent. @return Zero or negative errno;
 * failure leaves every count unchanged. No allocation or I/O occurs.
 */
int device_mapping_retain_range(struct device_mapping *mapping,uint64_t offset,uint64_t length)
{
    int rc=device_mapping_check_range(mapping,offset,length);
    if(rc || !mapping || !mapping->parent)return rc;
    uint64_t end=offset+length;
    for(struct device_policy_range *p=mapping->policy_cursor;p && p->offset<end;p=p->next) {
        DEVICE_POLICY_VISIT();++p->owners;
    }
    return 0;
}
/** @brief Count a view reference scheduled for whole-operation retirement.
 * @param mapping Borrowed view, nullable. The execution writer serializes the
 * paired marks; callers mark only references they already own.
 */
void device_mapping_retire_begin(struct device_mapping *mapping)
{
    if(mapping && mapping->parent) ++mapping->retiring_references;
}
/** @brief Remove a whole-operation retirement mark before any marked put.
 * @param mapping Borrowed previously marked view, nullable. Execution-owned
 * callers pair every mark; no allocation, lock or callback is involved.
 */
void device_mapping_retire_end(struct device_mapping *mapping)
{
    if(mapping && mapping->parent && mapping->retiring_references)
        --mapping->retiring_references;
}
/** @brief Drop page owners after retiring their user/kernel aliases.
 * @param mapping Retained view. @param offset Aligned backing start.
 * @param length Aligned extent. @param final_put Caller immediately puts view.
 * Last-page claims retire independently; when the final view is immediately
 * put, zero-owner claims survive through a possible final provider callback.
 */
void device_mapping_release_range(struct device_mapping *mapping,uint64_t offset,
                                  uint64_t length,bool final_put)
{
    if(!mapping || !mapping->parent || !length || (offset&4095u) || (length&4095u) ||
       offset>mapping->info.length || length>mapping->info.length-offset)return;
    uint64_t end=offset+length;
    bool defer=final_put && (mapping->references==1 ||
        (mapping->retiring_references && mapping->retiring_references==mapping->references));
    struct device_policy_range *p=device_policy_find(mapping,offset);
    while(p && p->offset<end) {
        DEVICE_POLICY_VISIT();
        struct device_policy_range *next=p->next;
        if(p->owners && --p->owners==0 && !defer) {
            if(p->previous)p->previous->next=next;else mapping->policies=next;
            if(next)next->previous=p->previous;else mapping->policy_tail=p->previous;
            mapping->policy_tree=device_policy_remove(mapping->policy_tree,p);
            paging_cache_claim_release(&p->claim);kernel_free(p);
        }
        p=next;
    }
    mapping->policy_cursor=p ? p : mapping->policy_tail;
    mapping->policy_high_end=mapping->policy_tail ? mapping->policy_tail->end : 0;
}
EXPORT_SYMBOL(device_mapping_create);
EXPORT_SYMBOL(device_mapping_get);
EXPORT_SYMBOL(device_mapping_put);
EXPORT_SYMBOL(device_mapping_physical);

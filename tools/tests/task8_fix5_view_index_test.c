/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Intrusive DEVICE view index against a sparse ownership oracle.
 * Real mapping code runs with a modeled physical-policy provider and heap. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/device/mapping.c"

static unsigned allocations,claims,releases,callback_claims;
static bool allocation_fails;
static uint32_t state=0xa321ed5bu;
/** @brief Allocate one modeled mapping object. @param size Bytes. @return Owned
 * memory or NULL at the injected allocation failure. */
void *kernel_malloc(size_t size)
{ if(allocation_fails)return NULL;void *p=malloc(size);if(p)++allocations;return p; }
/** @brief Free one modeled mapping object. @param p Owned memory, nullable. */
void kernel_free(void *p) { if(p){assert(allocations);--allocations;free(p);} }
/** @brief Select the modeled page policy. @param claim Empty owner. @param phys
 * Physical page. @param flags Receives policy. @return True after ownership. */
bool paging_cache_claim_select(struct paging_cache_claim *claim,uint64_t phys,uint64_t *flags)
{ assert(!claim->active && phys && !(phys&4095));claim->start=phys;claim->end=phys+4096;
  claim->flags=0;claim->active=true;*flags=0;++claims;return true; }
/** @brief Reject impossible legacy range merging in an admission view.
 * @param a Earlier owner. @param b Later owner. @return Never returns. */
bool paging_cache_claim_merge(struct paging_cache_claim *a,struct paging_cache_claim *b)
{ (void)a;(void)b;abort(); }
/** @brief Retire one modeled page policy. @param claim Active owner. */
void paging_cache_claim_release(struct paging_cache_claim *claim)
{ assert(claim->active && claims);claim->active=false;--claims; }
/** @brief Return one deterministic device page. @param context Backing cookie.
 * @param offset Aligned backing position. @return Physical page identity. */
static uint64_t physical(void *context,uint64_t offset)
{ assert(context==&claims);return 0x1000000000ULL+offset; }
/** @brief Record all policies still held at the provider's last callback.
 * @param context Backing cookie. */
static void release(void *context) { assert(context==&claims);callback_claims=claims;++releases; }
static const struct device_mapping_operations ops={.physical=physical,.release=release};
/** @brief Generate the next reproducible random word. @return Pseudorandom word. */
static uint32_t next_random(void)
{ state^=state<<13;state^=state>>17;state^=state<<5;return state; }
/** @brief Check tree ordering, balance, height and list membership recursively.
 * @param node Current subtree. @param previous Previous in-order node.
 * @param count Accumulated nodes. @return Actual height. */
static int check_tree(struct device_policy_range *node,struct device_policy_range **previous,unsigned *count)
{
    if(!node)return 0;
    int l=check_tree(node->left,previous,count);
    if(*previous)assert((*previous)->offset<node->offset && (*previous)->next==node && node->previous==*previous);
    else assert(!node->previous);
    *previous=node;++*count;
    int r=check_tree(node->right,previous,count);
    assert(l-r<=1 && r-l<=1 && node->height==1+(l>r ? l : r));
    return node->height;
}
/** @brief Compare arbitrary admissions, holes and retirement against an oracle.
 * @return Zero after all nodes, backing and heap objects retire. */
int main(void)
{
    enum { N=2048, BASE=1000, STEPS=12000 };
    bool live[N]={0};unsigned count=0;
    struct device_mapping_info info={.length=8192ULL*4096,.max_prot=3,.can_split=true,.can_move=true};
    struct device_mapping *mapping=NULL;assert(!device_mapping_create(NULL,&ops,&claims,&info,&mapping));
    assert(!device_mapping_admit_view(&mapping));
    for(unsigned step=0;step<STEPS;++step) {
        device_mapping_test_tree_visits=0;
        unsigned i=next_random()%N;uint64_t off=(uint64_t)(BASE+i)*4096,flags=~0ULL;
        if(live[i]) {
            assert(!device_mapping_retain_range(mapping,off,4096));
            device_mapping_release_range(mapping,off,4096,false);
            assert(!device_mapping_check_range(mapping,off,4096));
            device_mapping_release_range(mapping,off,4096,false);live[i]=false;--count;
        } else {
            if(!(step%127)) {
                unsigned before=allocations;allocation_fails=true;
                assert(device_mapping_cache_page(mapping,off,physical(&claims,off),&flags)==-ENOMEM);
                allocation_fails=false;assert(allocations==before && claims==count);
            }
            assert(!device_mapping_cache_page(mapping,off,physical(&claims,off),&flags) && !flags);
            live[i]=true;++count;
        }
        assert(device_mapping_test_tree_visits<80);
        unsigned query=next_random()%N,j=query;while(j<N && !live[j])++j;
        uint64_t query_offset=(uint64_t)(BASE+query)*4096;
        device_mapping_test_visits=0;
        struct device_policy_range *found=device_policy_find(mapping,query_offset);
        assert((j==N && !found) || (j<N && found && found->offset==(uint64_t)(BASE+j)*4096));
        assert(device_mapping_test_visits<40 && claims==count && allocations==count+2);
        if(!(step%32)) {
            struct device_policy_range *previous=NULL;unsigned indexed=0;
            int height=check_tree(mapping->policy_tree,&previous,&indexed);
            assert(indexed==count && previous==mapping->policy_tail && height<32);
        }
    }
    callback_claims=0;device_mapping_put(mapping);
    assert(releases==1 && callback_claims==count && !claims && !allocations);
    puts("PASS sparse view AVL, reverse/hole lookups, retained owners, OOM and final callback");
    return 0;
}

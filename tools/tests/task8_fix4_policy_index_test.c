/* SPDX-License-Identifier: Apache-2.0 */
/* Actual physical-policy index versus an independent overlap oracle. */
#define main task8_fix4_inherited_ioremap_main
#include "ioremap_test.c"
#undef main

/** @brief Model local invalidation without a privileged host instruction.
 * @param address Virtual page invalidated by production paging code.
 */
void x86_64_invlpg(uint64_t address) { (void)address; }
static uint32_t random_state=0x967ab531u;
/** @brief Generate a reproducible test value. @return Next pseudo-random word. */
static uint32_t next_random(void)
{ random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state; }

/** @brief Check the index invariants after arbitrary real owner mutations.
 * @param node Subtree root, nullable. @param previous Previous in-order node.
 * @param count Accumulated indexed owner count. @param maximum Subtree endpoint.
 * @return Actual subtree height.
 */
static int check_tree(struct paging_cache_claim *node,struct paging_cache_claim **previous,
                      unsigned *count,uint64_t *maximum)
{
    if(!node){*maximum=0;return 0;}
    uint64_t left_end,right_end;
    int left=check_tree(node->left,previous,count,&left_end);
    if(*previous)assert(cache_tree_compare(*previous,node)<0);
    *previous=node;++*count;
    int right=check_tree(node->right,previous,count,&right_end);
    assert(left-right<=1 && right-left<=1);
    assert(node->tree_height==1+(left>right?left:right));
    *maximum=node->end;
    if(left_end>*maximum)*maximum=left_end;
    if(right_end>*maximum)*maximum=right_end;
    assert(node->subtree_end==*maximum && node->active);
    return node->tree_height;
}

/** @brief Compare indexed policy admission/query and retirement with a list oracle.
 * @return Zero after all randomized and merged owners are retired.
 */
int main(void)
{
    enum { OWNER_COUNT=1024, STEPS=8000 };
    const uint64_t base_phys=0xc000000000ULL,uc=RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD;
    struct paging_cache_claim *owners=calloc(OWNER_COUNT,sizeof(*owners));assert(owners);
    unsigned live=0;
    for(unsigned step=0;step<STEPS;++step) {
        unsigned index=next_random()%OWNER_COUNT;
        struct paging_cache_claim *owner=&owners[index];
        if(owner->active) {paging_cache_claim_release(owner);--live;}
        else {
            uint64_t start=base_phys+(uint64_t)(next_random()%2048)*4096;
            uint64_t bytes=(uint64_t)(1+next_random()%8)*4096;
            uint64_t flags=(next_random()&1)?uc:0;
            bool compatible=true;
            for(unsigned i=0;i<OWNER_COUNT;++i)
                if(owners[i].active && owners[i].start<start+bytes && start<owners[i].end && owners[i].flags!=flags)
                    compatible=false;
            assert(paging_cache_claim_exact(owner,start,bytes,flags)==compatible);
            if(compatible)++live;
        }
        uint64_t start=base_phys+(uint64_t)(next_random()%2048)*4096,end=start+(uint64_t)(1+next_random()%8)*4096;
        uint64_t flags=(next_random()&1)?uc:0;
        bool any=false,compatible=true;
        for(unsigned i=0;i<OWNER_COUNT;++i)if(owners[i].active && owners[i].start<end && start<owners[i].end) {
            any=true;if(owners[i].flags!=flags)compatible=false;
        }
        kernel_spin_lock(&mmio_lock);
        assert(cache_claim_overlaps(start,end)==any);
        assert(cache_claim_compatible(start,end,flags)==compatible);
        kernel_spin_unlock(&mmio_lock);
        if(!(step%32)) {
            struct paging_cache_claim *previous=NULL;unsigned count=0;uint64_t maximum;
            (void)check_tree(cache_claim_tree,&previous,&count,&maximum);assert(count==live);
        }
    }
    for(unsigned i=0;i<OWNER_COUNT;++i)paging_cache_claim_release(&owners[i]);
    assert(!cache_claims && !cache_claim_tree);
    struct paging_cache_claim a={0},b={0};
    assert(paging_cache_claim_exact(&a,base_phys,4096,uc));
    assert(paging_cache_claim_exact(&b,base_phys+4096,4096,uc));
    assert(paging_cache_claim_merge(&a,&b));
    kernel_spin_lock(&mmio_lock);
    assert(cache_claim_overlaps(base_phys+4096,base_phys+8192));
    assert(!cache_claim_compatible(base_phys+4096,base_phys+8192,0));
    kernel_spin_unlock(&mmio_lock);
    paging_cache_claim_release(&a);assert(!cache_claims && !cache_claim_tree);
    free(owners);puts("PASS indexed policy against sparse/overlap randomized oracle and merge");
    return 0;
}

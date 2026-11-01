/* SPDX-License-Identifier: Apache-2.0 */
/* Actual page-table implementation with the existing host physical model. */
#define main task8_inherited_page_model_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "private_anon_fault_test.c"
#pragma clang diagnostic pop
#undef main

void task8_page_model_current(struct task *task)
{ current_task=task;nx_enabled=true; }
uint32_t mm_page_refcount(uint64_t physical)
{
    for(unsigned i=0;i<sizeof(host_page_refs)/sizeof(host_page_refs[0]);++i)
        if(host_page_refs[i].physical==physical)return host_page_refs[i].references;
    return 0;
}
void page_cache_mark_shared_writable(uint64_t physical) { (void)physical;abort(); }

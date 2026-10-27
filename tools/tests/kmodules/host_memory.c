#define _GNU_SOURCE
#include "host_memory.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/module_memory.h>
#include <reliefnt/lock.h>
#include <sched.h>
long host_fail_after=-1;
size_t host_live_heap,host_live_maps;
void *kernel_malloc(size_t n)
{
    if(host_fail_after==0)return NULL;
    if(host_fail_after>0)--host_fail_after;
    void *p=calloc(1,n);
    if(p)__atomic_add_fetch(&host_live_heap,1,__ATOMIC_RELAXED);
    return p;
}
void kernel_free(void *p)
{
    if(p){__atomic_sub_fetch(&host_live_heap,1,__ATOMIC_RELAXED);free(p);}
}
int module_memory_alloc(size_t n,size_t align,struct module_memory **out)
{
    assert(align<=4096);
    struct module_memory *m=kernel_malloc(sizeof(*m));if(!m)return -12;
    m->size=(n+4095)&~(size_t)4095;
    void *p=mmap(NULL,m->size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED){kernel_free(m);return -12;}
    m->address=(uintptr_t)p;*out=m;
    __atomic_add_fetch(&host_live_maps,1,__ATOMIC_RELAXED);return 0;
}
int module_memory_protect(struct module_memory *m,unsigned p)
{
    assert(p!=(MODULE_MEMORY_WRITE|MODULE_MEMORY_EXEC));
    int prot=PROT_READ;
    if(p&MODULE_MEMORY_WRITE)prot|=PROT_WRITE;
    if(p&MODULE_MEMORY_EXEC)prot|=PROT_EXEC;
    return mprotect((void *)m->address,m->size,prot) ? -13 : 0;
}
void module_memory_free(struct module_memory *m)
{
    if(m){assert(!munmap((void *)m->address,m->size));__atomic_sub_fetch(&host_live_maps,1,__ATOMIC_RELAXED);kernel_free(m);}
}
void kernel_spin_lock_irqsave(struct kernel_spinlock *l,uint64_t *f)
{
    *f=0;while(__atomic_exchange_n(&l->state,1,__ATOMIC_ACQUIRE))sched_yield();
}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *l,uint64_t f)
{
    (void)f;__atomic_store_n(&l->state,0,__ATOMIC_RELEASE);
}

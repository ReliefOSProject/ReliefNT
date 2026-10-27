/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../../kernel/device/manager_internal.h"
#include <reliefnt/driver_module.h>

static _Thread_local unsigned execution_depth;
static unsigned starts, exits, irq_releases, data_releases, detaches;
static int init_result, stop_result, resource_error;
static atomic_bool callback_active;
static struct { int (*stop)(void *); void (*free)(void *); void *context; } resource;
static struct module owner;
static unsigned execution_pins;
bool module_execution_get(struct module *code)
{ assert(!code || code==&owner); if(code)++execution_pins; return true; }
void module_execution_put(struct module *code)
{ assert(!code || (code==&owner && execution_pins)); if(code)--execution_pins; }
void kernel_spin_lock(struct kernel_spinlock *lock)
{ while(__atomic_exchange_n(&lock->state,1,__ATOMIC_ACQUIRE)) {} }
void kernel_spin_unlock(struct kernel_spinlock *lock) { __atomic_store_n(&lock->state,0,__ATOMIC_RELEASE); }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags) { *flags=0; kernel_spin_lock(lock); }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags) { (void)flags; kernel_spin_unlock(lock); }
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags=0; ++execution_depth; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; assert(execution_depth); --execution_depth; }
int kernel_execution_suspend_irqrestore(struct kernel_execution_suspend_token *token)
{ assert(execution_depth==1 && !token->active); execution_depth=0; token->active=1; return 0; }
int kernel_execution_resume_irqsave(struct kernel_execution_suspend_token *token)
{ assert(!execution_depth && token->active); execution_depth=1; token->active=0; return 0; }
void console_log(enum console_log_level level,const char *format,...) { (void)level; (void)format; }
__attribute__((noreturn)) void panic(const char *text) { fprintf(stderr,"panic: %s\n",text); abort(); }
int sound_service_unregister_owner(uint32_t id,uint32_t force) { (void)id; (void)force; assert(!execution_depth); return stop_result; }
void driver_resources_release(uint32_t id,bool irq_only)
{ (void)id; if(irq_only){assert(!execution_depth);++irq_releases;} else {assert(execution_depth);++data_releases;} }
void net_driver_detached(void) { ++detaches; }
int module_add_resource(struct module *code,int (*stop)(void *),void (*free_fn)(void *),void *context)
{
    assert(code==&owner && stop && free_fn && !resource.stop);
    if(resource_error) return resource_error;
    resource.stop=stop; resource.free=free_fn; resource.context=context; return 0;
}
static int ready(void) { return 1; }
static const uint8_t *mac(void) { static const uint8_t address[6]={2,0,0,0,0,1}; return address; }
static int send_frame(const void *bytes,uint32_t length) { (void)bytes; return (int)length; }
static int poll_frame(void *bytes,uint32_t size,uint32_t *length) { (void)bytes; (void)size; *length=0; return 0; }
static void info(struct reliefos_driver_e1000_info *out) { *out=(struct reliefos_driver_e1000_info){0}; }
static const struct reliefos_driver_e1000_ops ops={ready,mac,send_frame,poll_frame,info};
static const struct reliefos_driver_kernel_api api={.abi_version=RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size=sizeof(api),.register_e1000=driver_register_e1000};
const struct reliefos_driver_kernel_api *driver_kernel_services(void) { return &api; }
static int start(const struct reliefos_driver_kernel_api *services)
{ assert(!execution_depth && services==&api); ++starts; return init_result ? init_result : services->register_e1000(&ops); }
static void stop(void) { assert(!execution_depth && !atomic_load(&callback_active)); ++exits; }
static const struct reliefos_driver_module descriptor={.magic=RELIEFOS_DRIVER_MODULE_MAGIC,
    .abi_version=RELIEFOS_DRIVER_ABI_VERSION,.struct_size=sizeof(descriptor),
    .kind=RELIEFOS_DRIVER_KIND_NETWORK,.name="fixture_network",.init=start,.fini=stop};
static void release_binding(void)
{ assert(!resource.stop(resource.context)); resource.free(resource.context); resource=(__typeof__(resource)){0}; }
static void *finish_callback(void *unused)
{
    (void)unused;
    struct timespec delay={0,20000000}; nanosleep(&delay,NULL);
    atomic_store(&callback_active,false); driver_manager_service_unpin(0); return NULL;
}
int main(void)
{
    uint64_t flags; kernel_execution_lock_irqsave(&flags);
    driver_manager_init();
    resource_error=-12;
    assert(driver_module_register(&descriptor,&owner)==-12 && !starts && !resource.stop);
    resource_error=0;
    assert(!driver_module_register(&descriptor,&owner) && starts==1 && resource.stop);
    assert(driver_slots[0].code_owner==&owner && !(driver_slots[0].info.flags&RELIEFOS_DRIVER_FLAG_BUILTIN));
    assert(e1000_is_ready());
    assert(driver_module_register(&descriptor,&owner)==-17 && starts==1);
    stop_result=-16; assert(resource.stop(resource.context)==-16);
    assert(!exits && driver_slots[0].module && !e1000_is_ready());
    stop_result=0;
    /* Reopen only to create one already-admitted callback before retry. */
    driver_manager_service_reopen(0); assert(driver_manager_service_try_pin(0));
    atomic_store(&callback_active,true); pthread_t thread;
    assert(!pthread_create(&thread,NULL,finish_callback,NULL));
    release_binding(); assert(!pthread_join(thread,NULL));
    assert(exits==1 && !e1000_is_ready() && !driver_slots[0].info.file[0] && !execution_pins);
    assert(irq_releases && data_releases && detaches==1);
    init_result=-19; assert(!driver_module_register(&descriptor,&owner));
    assert(starts==2 && !e1000_is_ready()); release_binding();
    init_result=-12; assert(driver_module_register(&descriptor,&owner)==-12);
    release_binding(); assert(!driver_slots[0].info.file[0]);
    init_result=0; assert(!driver_module_register(&descriptor,NULL));
    assert(e1000_is_ready() && !resource.stop && (driver_slots[0].info.flags&RELIEFOS_DRIVER_FLAG_BUILTIN));
    kernel_execution_unlock_irqrestore(flags);
    puts("PASS native driver owner bindings: reserve-before-init, duplicate, callback drain, failed STOP retained, absent hardware, rollback and builtin path");
}

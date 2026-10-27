/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/module.h>
#include <reliefnt/console.h>

extern const struct module_initcall __start_reliefnt_initcalls[];
extern const struct module_initcall __stop_reliefnt_initcalls[];
extern const struct module_initcall __start_reliefnt_subsys_initcalls[];
extern const struct module_initcall __stop_reliefnt_subsys_initcalls[];
extern const struct module_initcall __start_reliefnt_fs_initcalls[];
extern const struct module_initcall __stop_reliefnt_fs_initcalls[];
extern const struct module_initcall __start_reliefnt_block_initcalls[];
extern const struct module_initcall __stop_reliefnt_block_initcalls[];
static bool block_initcalls_started;
static int block_initcalls_result;
static bool initcalls_started;
static int initcalls_result;
static bool fs_initcalls_started;
static int fs_initcalls_result;

/** @brief Run a linker-owned phase without a lock or central implementation list.
 * @param first Inclusive immutable phase start. @param last Exclusive phase end.
 * @return First negative initializer result or zero; every record is visited.
 * Boot CPU only with phase reentry excluded by caller; no callback lock held.
 */
static int run_initcall_phase(const struct module_initcall *first,const struct module_initcall *last)
{
    int result=0;
    for(const struct module_initcall *entry=first;entry<last;++entry) {
        int status=entry->init ? entry->init() : -22;
        if(status<0) {
            if(!result)result=status;
            console_warn("[module] builtin %s initialization failed: %d\n",entry->name,status);
        }
    }
    return result;
}

/** @brief Register builtin filesystem providers before root-volume probing.
 * @return First negative FS initializer result or zero, retained on later calls.
 * Boot CPU only after heap/IRQ setup; no module lock spans provider code.
 * Ordinary device initcalls remain untouched until their later boot phase.
 */
int module_run_fs_initcalls(void)
{
    if(fs_initcalls_started)return fs_initcalls_result;
    fs_initcalls_started=true;
    fs_initcalls_result=run_initcall_phase(__start_reliefnt_fs_initcalls,__stop_reliefnt_fs_initcalls);
    return fs_initcalls_result;
}

/** @brief Run early block providers exactly once before root construction.
 * @return First negative initializer result or zero; repeated calls retain it.
 * BSP under the caller's depth-one execution transaction after IRQ/manager
 * setup. Native registration may suspend execution for hardware waits; no
 * module/registry lock is held while visiting the immutable linker records.
 */
int module_run_block_initcalls(void)
{
    if(block_initcalls_started)return block_initcalls_result;
    block_initcalls_started=true;
    block_initcalls_result=run_initcall_phase(__start_reliefnt_block_initcalls,__stop_reliefnt_block_initcalls);
    return block_initcalls_result;
}

/** @brief Run linked subsystems before devices without a central implementation list.
 * @return First negative init result, or zero; later calls return the same result.
 * Boot CPU only, serially before userspace/AP execution. Records are immutable
 * linker-owned storage. Every subsystem/device initializer runs even after a predecessor fails;
 * builtin exits are never invoked. The caller supplies the boot execution context.
 */
int module_run_builtin_initcalls(void)
{
    if(initcalls_started) return initcalls_result;
    initcalls_started=true;
    initcalls_result=run_initcall_phase(__start_reliefnt_subsys_initcalls,__stop_reliefnt_subsys_initcalls);
    int devices=run_initcall_phase(__start_reliefnt_initcalls,__stop_reliefnt_initcalls);
    if(!initcalls_result)initcalls_result=devices;
    return initcalls_result;
}

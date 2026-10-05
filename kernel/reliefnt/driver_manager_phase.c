#include <reliefnt/driver_manager_phase.h>
#include <reliefos/driver.h>
#include <reliefnt/audio.h>

static uint32_t manager_admitted;
static struct kernel_spinlock service_lock = KERNEL_SPINLOCK_INIT;
static uint32_t service_enabled[RELIEFOS_DRIVER_MAX];
static uint32_t service_closing[RELIEFOS_DRIVER_MAX];
static uint32_t service_calls[RELIEFOS_DRIVER_MAX];

/**
 * @brief Admit one manager lifecycle operation without blocking an execution-lock holder.
 * @return 0 when admitted, or -16 while another lifecycle operation is active.
 */
int driver_manager_phase_try_enter(void)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&manager_admitted, &expected, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : -16;
}

/**
 * @brief Release the lifecycle admission acquired by driver_manager_phase_try_enter.
 * @return None. The caller owns the manager admission gate.
 */
void driver_manager_phase_leave(void)
{
    __atomic_store_n(&manager_admitted, 0, __ATOMIC_RELEASE);
}
/** @brief Process deferred audio work before admitting a syscall's I/O pin.
 * @return None. Task context with depth-one execution ownership. Nonblocking
 * manager admission protects module images; STOP/close and card task callbacks
 * run with execution ownership suspended and IRQs enabled. Busy or failed
 * admission preserves queued work; task callbacks share a total budget of 32.
 */
void driver_manager_process_audio_faults(void)
{
    if((!audio_disconnect_work_pending() && !audio_pcm_release_work_pending() &&
        !audio_task_work_pending()) ||
        driver_manager_phase_try_enter())return;
    struct driver_manager_wait_scope scope={0};
    if(!driver_manager_wait_begin(&scope)){
        (void)audio_process_pending_disconnects(16u);
        (void)audio_pcm_retry_releases(32u);
        (void)audio_process_task_work(32u);
        if(!driver_manager_wait_end(&scope))audio_pcm_finish_releases();
    }
    driver_manager_phase_leave();
}

/**
 * @brief Release execution ownership and enable IRQ progress for a manager waitable phase.
 * @param scope Zero-initialized storage that receives the active execution suspension token.
 * @return 0 when suspended or a negative errno when input/ownership is invalid.
 */
int driver_manager_wait_begin(struct driver_manager_wait_scope *scope)
{
    if (!scope || scope->active) return -22;
    int ret = kernel_execution_suspend_irqrestore(&scope->execution);
    if (ret < 0) return ret;
    scope->active = 1;
    return 0;
}

/**
 * @brief Reacquire execution ownership with IRQs masked after a manager waitable phase.
 * @param scope Active scope returned by driver_manager_wait_begin.
 * @return 0 when resumed or a negative errno when the token cannot be resumed.
 */
int driver_manager_wait_end(struct driver_manager_wait_scope *scope)
{
    if (!scope || !scope->active) return -22;
    int ret = kernel_execution_resume_irqsave(&scope->execution);
    if (ret < 0) return ret;
    scope->active = 0;
    return 0;
}

/**
 * @brief Publish one loaded module's v1 callback admission.
 * @param owner Loaded manager slot index.
 * @return None. The service state is changed under an IRQ-safe registry lock.
 */
void driver_manager_service_enable(uint32_t owner)
{
    if (owner >= RELIEFOS_DRIVER_MAX) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&service_lock, &flags);
    service_closing[owner] = 0;
    service_enabled[owner] = 1;
    kernel_spin_unlock_irqrestore(&service_lock, flags);
}

/**
 * @brief Close admission before draining module-owned v1 callbacks.
 * @param owner Driver slot whose callbacks are being retired.
 * @return None. Existing pins remain counted until callback return.
 */
void driver_manager_service_close(uint32_t owner)
{
    if (owner >= RELIEFOS_DRIVER_MAX) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&service_lock, &flags);
    service_closing[owner] = 1;
    kernel_spin_unlock_irqrestore(&service_lock, flags);
}

/**
 * @brief Restore callback admission when unload fails and retains the module image.
 * @param owner Still-loaded driver slot.
 * @return None. Existing module callbacks remain valid.
 */
void driver_manager_service_reopen(uint32_t owner)
{
    if (owner >= RELIEFOS_DRIVER_MAX) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&service_lock, &flags);
    if (service_enabled[owner]) service_closing[owner] = 0;
    kernel_spin_unlock_irqrestore(&service_lock, flags);
}

/**
 * @brief Permanently close callback admission after all module code has stopped.
 * @param owner Driver slot being cleared.
 * @return None. Caller has already drained every active pin.
 */
void driver_manager_service_disable(uint32_t owner)
{
    if (owner >= RELIEFOS_DRIVER_MAX) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&service_lock, &flags);
    service_closing[owner] = 1;
    service_enabled[owner] = 0;
    kernel_spin_unlock_irqrestore(&service_lock, flags);
}

/**
 * @brief Admit one callback only while its owning module is published and open.
 * @param owner Driver slot from the manager callback table.
 * @return True with a live reference, or false when callback admission is closed.
 */
bool driver_manager_service_try_pin(uint32_t owner)
{
    if (owner >= RELIEFOS_DRIVER_MAX) return false;
    uint64_t flags;
    kernel_spin_lock_irqsave(&service_lock, &flags);
    bool admitted = service_enabled[owner] && !service_closing[owner];
    if (admitted) __atomic_add_fetch(&service_calls[owner], 1, __ATOMIC_ACQUIRE);
    kernel_spin_unlock_irqrestore(&service_lock, flags);
    return admitted;
}

/**
 * @brief Release one callback reference so the manager may finalize its module.
 * @param owner Driver slot whose callback returned.
 * @return None. The atomic decrement pairs with service drain's acquire load.
 */
void driver_manager_service_unpin(uint32_t owner)
{
    if (owner < RELIEFOS_DRIVER_MAX)
        __atomic_sub_fetch(&service_calls[owner], 1, __ATOMIC_RELEASE);
}

/**
 * @brief Wait until all admitted legacy callbacks for a closed owner have returned.
 * @param owner Driver slot whose callbacks are being drained.
 * @return None. Task context without execution ownership or service registry lock.
 */
void driver_manager_service_drain(uint32_t owner)
{
    if (owner >= RELIEFOS_DRIVER_MAX) return;
    while (__atomic_load_n(&service_calls[owner], __ATOMIC_ACQUIRE) != 0)
        __asm__ volatile("pause" ::: "memory");
}

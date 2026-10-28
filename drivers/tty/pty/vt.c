#include <reliefnt/graphics_service.h>
#include "internal.h"

/**
 * @brief Deliver one process-directed VT controller signal, not a group broadcast.
 * @param signal_number Registered signal; zero is an existence probe.
 * @param process_id Controller TGID, zero if none.
 * @return Zero if delivered, negative ESRCH or EINVAL when delivery fails.
 */
static int pty_vt_signal(short signal_number, uint32_t process_id);

/**
 * @brief Commit a display switch and notify the destination controller.
 * @param number Destination fixed VT number, one through six.
 * @return Zero on success or negative EINVAL for an invalid console.
 *
 * Runs only once the outgoing VT_PROCESS controller has granted the release
 * with VT_RELDISP(1), or immediately when the outgoing console is VT_AUTO.
 */
static int pty_vt_commit_switch(uint32_t number);


/** @brief Reserve tty1 through tty6 before user processes start.
 * @return Zero on success, negative ENOMEM if allocation fails. */
int pty_vt_init(void)
{
    if (vt_ready) return 0;
    for (uint32_t number = 1; number <= VT_COUNT; ++number) {
        int32_t id = pty_create(0);
        if (id != (int32_t)number) return -12;
        struct pty_session *session = pty_session_slot_at(number - 1u);
        if (!session) return -12;
        session->console = 1;
        if (console_vga_text_mode) {
            session->winsize.ws_row=25;
            session->winsize.ws_col=80;
        }
        session->locked = 0;
        vt_modes[number - 1u] = (struct vt_mode){ .mode = VT_AUTO };
        vt_keyboard_modes[number - 1u] = K_XLATE;
        vt_release_pending[number - 1u] = 0;
        vt_acquire_pending[number - 1u] = 0;
        vt_controller_pid[number - 1u] = 0;
        vt_relsig[number - 1u] = 0;
        vt_acqsig[number - 1u] = 0;
    }
    vt_switch_target = 0;
    vt_display_generation = 1;
    active_vt = 1;
    vt_ready = 1;
    return 0;
}

/**
 * @brief Deliver one process-directed VT controller signal, not a group broadcast.
 * @param signal_number Registered signal; zero is an existence probe.
 * @param process_id Controller TGID, zero if none.
 * @return Zero if delivered, negative ESRCH or EINVAL when delivery fails.
 */
static int pty_vt_signal(short signal_number, uint32_t process_id)
{
    if (!process_id) return -3;
    if (signal_number < 0 || (unsigned)signal_number >= KERNEL_SIGNAL_ACTION_MAX) return -22;
    return sched_signal_user_process(process_id, (int)signal_number);
}

/** @brief Read the Linux VT ownership mode for a fixed console. */
int pty_vt_get_mode(uint32_t pty_id, struct vt_mode *mode)
{
    if (!mode || !pty_vt_number(pty_id)) return -22;
    *mode = vt_modes[pty_id - 1u];
    return 0;
}

/**
 * @brief Set the Linux VT ownership mode for a fixed console.
 * @param pty_id Fixed terminal identifier, one through six.
 * @param mode Requested mode; only VT_AUTO and VT_PROCESS are accepted.
 * @return Zero on success, negative EINVAL for an invalid console or mode.
 *
 * Linux treats VT_ACKACQ as a one-shot VT_RELDISP acknowledgement and never as
 * a persistent ownership mode, so it is rejected here.  Claiming VT_PROCESS
 * records the caller's TGID and its release/acquire signals; returning
 * to VT_AUTO releases the claim and abandons any hand-off in flight.
 */
int pty_vt_set_mode(uint32_t pty_id, const struct vt_mode *mode)
{
    uint32_t index;
    if (!mode || !pty_vt_number(pty_id)) return -22;
    if (mode->mode != VT_AUTO && mode->mode != VT_PROCESS) return -22;
    index = pty_id - 1u;
    vt_modes[index] = *mode;
    vt_modes[index].frsig = 0; /* Linux ignores forced-release signals. */
    if (vt_release_pending[index]) vt_switch_target = 0;
    if (mode->mode == VT_AUTO) {
        uint8_t owned_handoff = vt_release_pending[index];
        vt_release_pending[index] = 0;
        vt_acquire_pending[index] = 0;
        vt_controller_pid[index] = 0;
        vt_relsig[index] = 0;
        vt_acqsig[index] = 0;
        if (owned_handoff) vt_switch_target = 0;
    } else {
        const struct task *caller = sched_current_task();
        vt_controller_pid[index] =
            caller ? (caller->tgid ? caller->tgid : caller->pid) : 0;
        vt_relsig[index] = mode->relsig;
        vt_acqsig[index] = mode->acqsig;
        vt_release_pending[index] = 0;
        vt_acquire_pending[index] = 0;
    }
    console_info("[reliefnt] VT mode vt=%u mode=%d relsig=%d acqsig=%d\n",
                   pty_id, (int)vt_modes[index].mode, (int)vt_modes[index].relsig,
                   (int)vt_modes[index].acqsig);
    return 0;
}

/**
 * @brief Return a fixed virtual console to its default Linux ownership state.
 * @param pty_id Fixed terminal identifier, one through six.
 * @return Zero on success, negative EINVAL for an invalid console.
 *
 * Controller recovery cancels the hand-off, restores KD_TEXT and the default
 * keyboard mode. Plain VT_SETMODE(VT_AUTO) does not reset KD or keyboard mode.
 */
int pty_vt_reset_mode(uint32_t pty_id)
{
    uint32_t index;
    uint8_t owned_handoff;
    uint8_t was_graphical;
    if (!pty_vt_number(pty_id)) return -22;
    index = pty_id - 1u;
    owned_handoff = vt_release_pending[index];
    was_graphical = vt_graphical[index];
    if (console_vga_text_mode && was_graphical && active_vt==pty_id) {
        int ret=console_prepare_vt(false);
        if (ret) return ret;
    }
    vt_modes[index] = (struct vt_mode){ .mode = VT_AUTO };
    vt_release_pending[index] = 0;
    vt_acquire_pending[index] = 0;
    vt_controller_pid[index] = 0;
    vt_relsig[index] = 0;
    vt_acqsig[index] = 0;
    if (owned_handoff) vt_switch_target = 0;
    vt_graphical[index] = 0;
    vt_keyboard_modes[index] = K_XLATE;
    if (was_graphical && active_vt == pty_id) {
        graphics_service_kms_vt_release();
        ++vt_display_generation;
        input_set_graphical_vt(0);
        console_vt_activate(pty_id, false);
    }
    (void)kernel_wait_queue_wake_all(&vt_waiters);
    return 0;
}

/** @brief Read the Linux virtual-console keyboard translation mode. */
int pty_vt_get_keyboard_mode(uint32_t pty_id, int *mode)
{
    if (!mode || !pty_vt_number(pty_id)) return -22;
    *mode = vt_keyboard_modes[pty_id - 1u];
    return 0;
}

/**
 * @brief Set the Linux virtual-console keyboard translation mode.
 * @param pty_id Fixed terminal identifier, one through six.
 * @param mode One of K_RAW, K_XLATE, K_MEDIUMRAW, K_UNICODE or K_OFF.
 * @return Zero on success, negative EINVAL for an invalid console or mode.
 */
int pty_vt_set_keyboard_mode(uint32_t pty_id, int mode)
{
    if (!pty_vt_number(pty_id) ||
        (mode != K_RAW && mode != K_XLATE && mode != K_MEDIUMRAW &&
         mode != K_UNICODE && mode != K_OFF))
        return -22;
    vt_keyboard_modes[pty_id - 1u] = mode;
    return 0;
}

/**
 * @brief Build the Linux VT_GETSTATE occupancy bitmap.
 * @return Bit zero for tty0 and bit n for each referenced console n.
 */
uint32_t pty_vt_state_bitmap(void)
{
    uint32_t state = 1;
    if (!vt_ready) return 0;
    for (uint32_t i = 0; i < VT_COUNT; ++i) {
        if (sched_pty_reference_count(i + 1u)) state |= 1u << (i + 1u);
    }
    return state;
}

/**
 * @brief Return the first virtual console available for allocation.
 * @return First unreferenced fixed console number, or -1 if none is available.
 */
int pty_vt_open_query(void)
{
    if (!vt_ready) return -1;
    for (uint32_t i = 0; i < VT_COUNT; ++i) {
        if (sched_pty_reference_count(i + 1u)) continue;
        return (int)(i + 1u);
    }
    return -1;
}

/**
 * @brief Commit a display switch and notify the destination controller.
 * @param number Destination fixed VT number, one through six.
 * @return Zero on success or negative EINVAL for an invalid console.
 *
 * Runs only once the outgoing VT_PROCESS controller has granted the release
 * with VT_RELDISP(1), or immediately when the outgoing console is VT_AUTO.
 */
static int pty_vt_commit_switch(uint32_t number)
{
    if (!pty_vt_id(number)) return -22;
    if (console_vga_text_mode) {
        int ret=console_prepare_vt(vt_graphical[number-1]!=0);
        if (ret) return ret;
    }
    if (active_vt && active_vt <= VT_COUNT) vt_release_pending[active_vt - 1u] = 0;
    vt_switch_target = 0;
    ++vt_display_generation;
    if (active_vt && active_vt != number) graphics_service_kms_vt_release();
    active_vt = number;
    /* A grabbed graphical client can consume modifier releases while the
     * display is away. Do not carry stale Ctrl/Alt state into the next VT;
     * evdev consumers receive their independent modifier snapshot below. */
    console_modifier_keys = 0;
    console_shift_down = 0;
    console_ctrl_down = 0;
    console_alt_down = 0;
    console_info("[reliefnt] VT active %u graphics=%d\n", number,
                   vt_graphical[number - 1u] != 0);
    input_set_graphical_vt(vt_graphical[number - 1u] ? number : 0);
    console_vt_activate(number, vt_graphical[number - 1u] != 0);
    (void)kernel_wait_queue_wake_all(&vt_waiters);
    if (vt_modes[number - 1u].mode == VT_PROCESS) {
        /* The new controller keeps VT_PROCESS; the pending flag is cleared by
         * its own VT_RELDISP(VT_ACKACQ), never by rewriting the mode. */
        vt_acquire_pending[number - 1u] = 1;
        if (pty_vt_signal(vt_acqsig[number - 1u], vt_controller_pid[number - 1u]) != 0) {
            (void)pty_vt_reset_mode(number);
        } else {
            console_debug("[reliefnt] VT acquire signal sent for vt=%u\n", number);
        }
    }
    return 0;
}

/**
 * @brief Release or acknowledge a virtual-console hand-off.
 * @param pty_id Fixed terminal identifier of the acknowledging controller.
 * @param request Zero refuses a pending release; any nonzero value grants it.
 * @return Zero on success, negative EINVAL for invalid mode/console or a
 *         no-pending request other than VT_ACKACQ. Never returns EAGAIN.
 *
 * Linux uses three VT_RELDISP requests for a VT_PROCESS owner: zero refuses a
 * pending release and leaves the active VT, graphics mode and input routing
 * untouched; one completes the switch and signals the destination controller;
 * With no release pending, Linux ignores VT_ACKACQ even if repeated or inactive.
 */
int pty_vt_release_display(uint32_t pty_id, int request)
{
    uint32_t index;
    uint32_t target;
    if (!pty_vt_number(pty_id)) return -22;
    index = pty_id - 1u;
    if (vt_modes[index].mode != VT_PROCESS) return -22;
    if (!vt_release_pending[index] || !vt_switch_target) {
        if (request != VT_ACKACQ) return -22;
        vt_acquire_pending[index] = 0;
        return 0;
    }
    if (request == 0) {
        /* VT_FALSE: refuse the release and keep the display as it is. */
        vt_release_pending[index] = 0;
        vt_switch_target = 0;
        return 0;
    }
    /* VT_TRUE: the controller has finished using its display. */
    target = vt_switch_target;
    /* A failed opt-in hardware transition must remain retryable: commit
     * clears the pending grant only after its prepare step succeeds. */
    if (console_vga_text_mode) return pty_vt_commit_switch(target);
    vt_release_pending[index] = 0;
    return pty_vt_commit_switch(target);
}

/** @brief Resolve a fixed VT after initialization.
 * @param number Virtual console number.
 * @return Terminal identifier, or zero for an invalid/uninitialized VT. */
uint32_t pty_vt_id(uint32_t number)
{
    return vt_ready && number >= 1u && number <= VT_COUNT ? number : 0u;
}

/** @brief Resolve a terminal to a fixed VT.
 * @param pty_id Terminal identifier.
 * @return VT number or zero for a dynamic PTY. */
uint32_t pty_vt_number(uint32_t pty_id)
{
    return pty_vt_id(pty_id);
}

/** @brief Read the selected virtual console.
 * @return One through six, or zero before initialization. */
uint32_t pty_vt_active(void)
{
    return active_vt;
}

/** @brief Read display invalidation state under the execution transaction.
 * @return Generation incremented by visible VT switches and mode activations.
 */
uint64_t pty_vt_generation(void)
{
    return vt_display_generation;
}

/**
 * @brief Request a display switch to a fixed virtual console.
 * @param number Destination virtual console number, one through six.
 * @return Zero on success or negative EINVAL for an invalid console.
 *
 * When the active console is owned by a VT_PROCESS controller this is a
 * request only: the display, graphics mode and input routing stay where they
 * are, the controller receives its registered release signal, and the switch is
 * committed later by VT_RELDISP(1).  A VT_AUTO console hands over immediately.
 */
int pty_vt_switch(uint32_t number)
{
    uint32_t old_vt;
    if (!pty_vt_id(number)) return -22;
    if (active_vt == number) return 0;
    old_vt = active_vt;
    console_info("[reliefnt] VT switch request %u -> %u\n", old_vt, number);
    if (old_vt && vt_modes[old_vt - 1u].mode == VT_PROCESS) {
        /* Xorg may request the same switch after the kernel has already
         * observed Ctrl+Alt+Fn.  Keep one in-flight hand-off and one signal;
         * a different target cannot replace the controller's pending grant. */
        if (vt_release_pending[old_vt - 1u]) {
            return vt_switch_target == number ? 0 : -16;
        }
        /* Two-phase hand-off: hold the display and ask the controller. */
        vt_release_pending[old_vt - 1u] = 1;
        vt_switch_target = number;
        if (pty_vt_signal(vt_relsig[old_vt - 1u], vt_controller_pid[old_vt - 1u]) == 0) {
            console_debug("[reliefnt] VT release signal sent for vt=%u\n", old_vt);
            return 0;
        }
        (void)pty_vt_reset_mode(old_vt);
    }
    /* Linux ignores VT_AUTO switches while the outgoing display is graphics. */
    if (old_vt && vt_graphical[old_vt - 1u]) {
        console_warn("[reliefnt] VT switch refused: vt=%u owns the display in graphics\n",
                       old_vt);
        return 0;
    }
    return pty_vt_commit_switch(number);
}

/**
 * @brief Wait for a VT under the syscall execution lock; switching wakes all waiters.
 * @param number Requested fixed VT number, one through six.
 * @return Zero if active, negative EINVAL, or KERNEL_SYSCALL_BLOCKED for retry.
 */
int64_t pty_vt_wait_active(uint32_t number)
{
    kernel_wait_queue_remove(&vt_waiters, sched_current_task());
    if (!pty_vt_id(number)) return -22;
    if (active_vt == number) return 0;
    kernel_wait_queue_block_current(&vt_waiters);
    return KERNEL_SYSCALL_BLOCKED;
}

/** @brief Change display mode under the execution transaction.
 * @param pty_id Fixed terminal identifier.
 * @param enabled One for graphics, zero for text.
 * @return Zero on success or negative EINVAL. */
int pty_vt_set_graphics(uint32_t pty_id, int enabled)
{
    if (!pty_vt_number(pty_id) || (enabled != 0 && enabled != 1)) return -22;
    if (console_vga_text_mode && active_vt==pty_id) {
        int ret=console_prepare_vt(enabled!=0);
        if (ret) return ret;
    }
    console_info("[reliefnt] VT graphics vt=%u enabled=%d\n", pty_id, enabled);
    vt_graphical[pty_id - 1u] = (uint8_t)enabled;
    if (active_vt == pty_id) {
        if (!enabled) graphics_service_kms_vt_release();
        ++vt_display_generation;
        input_set_graphical_vt(enabled ? pty_id : 0);
        console_vt_activate(pty_id, enabled != 0);
    }
    return 0;
}

/** @brief Inspect the selected display mode.
 * @return Nonzero if the active VT is graphical. */
int pty_vt_graphical_active(void)
{
    return active_vt && vt_graphical[active_vt - 1u] != 0;
}

/** @brief Inspect one fixed virtual console.
 * @param pty_id Fixed terminal identifier.
 * @return Nonzero if graphical, zero if text or invalid. */
int pty_vt_graphical(uint32_t pty_id)
{
    return pty_vt_number(pty_id) && vt_graphical[pty_id - 1u] != 0;
}

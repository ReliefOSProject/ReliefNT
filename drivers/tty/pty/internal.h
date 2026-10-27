#ifndef RELIEFNT_DRIVERS_TTY_PTY_PTY_C_INTERNAL_H
#define RELIEFNT_DRIVERS_TTY_PTY_PTY_C_INTERNAL_H

/*
 * ReliefOS kernel pseudo-terminals: implements terminal input and output queues.
 * Connects shells and terminal applications to their controlling sessions.
 */
#include <reliefnt/pty.h>
#include <reliefnt/input.h>
#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/sched.h>
#include <reliefnt/futex.h>
#include <reliefnt/wait.h>
#include <reliefos/psf_font.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <linux/input.h>

#define PTY_SESSION_PAGE_SIZE 32u
#define VT_COUNT 6u
#define PTY_INPUT_CAP 1024u
#define PTY_OUTPUT_CAP 8192u

struct pty_session {
    uint8_t used;
    uint8_t console;
    uint8_t hungup;
    uint8_t output_reported;
    uint8_t input_reported;
    uint8_t locked;
    uint32_t owner_pid;
    uint32_t generation;
    struct reliefos_permissions permissions;
    uint32_t process_session;
    uint32_t foreground_pgid;
    uint32_t transfer_refs;
    uint32_t transfer_masters;
    uint8_t input[PTY_INPUT_CAP];
    uint32_t input_head;
    uint32_t input_tail;
    /* ICANON input is kept separate until a line delimiter arrives. */
    uint8_t canonical_input[PTY_INPUT_CAP];
    uint32_t canonical_length;
    uint8_t output[PTY_OUTPUT_CAP];
    uint32_t output_head;
    uint32_t output_tail;
    /* OPOST processing tracks the cursor column for ONOCR/ONLRET/TAB3. */
    uint32_t output_column;
    struct reliefos_pty_termios termios;
    struct linux_winsize winsize;
};










/* VT hand-off signals are process-directed, never terminal job-control signals. */



/* Destination of the hand-off currently held on a VT_PROCESS controller. */

/* Set-1 modifier keys currently held, tracked per physical key so the derived
 * shift/ctrl/alt levels survive either side being released first. */
#define CONSOLE_KEY_LSHIFT (1U << 0)
#define CONSOLE_KEY_RSHIFT (1U << 1)
#define CONSOLE_KEY_LCTRL  (1U << 2)
#define CONSOLE_KEY_RCTRL  (1U << 3)
#define CONSOLE_KEY_LALT   (1U << 4)
#define CONSOLE_KEY_RALT   (1U << 5)


extern uint8_t vt_ready;
extern uint32_t active_vt;
extern uint8_t vt_graphical[VT_COUNT];
extern struct vt_mode vt_modes[VT_COUNT];
extern int vt_keyboard_modes[VT_COUNT];
extern uint8_t vt_release_pending[VT_COUNT];
extern uint8_t vt_acquire_pending[VT_COUNT];
extern uint32_t vt_controller_pid[VT_COUNT];
extern short vt_relsig[VT_COUNT];
extern short vt_acqsig[VT_COUNT];
extern uint32_t vt_switch_target;
extern uint32_t console_modifier_keys;
extern uint8_t console_shift_down;
extern uint8_t console_ctrl_down;
extern uint8_t console_alt_down;
extern struct kernel_wait_queue vt_waiters;
extern uint64_t vt_display_generation;

/**
 * @brief Resolve a live PTY session by its stable 1-based identifier.
 * @param pty_id PTY ID; zero is invalid.
 * @return Stable session pointer when allocated, otherwise NULL.
 */
struct pty_session *find_session(uint32_t pty_id);

/**
 * @brief Resolve a PTY session slot by zero-based stable ID index.
 * @param index Slot index where zero corresponds to PTY ID 1.
 * @return Stable slot pointer if its backing page exists, otherwise NULL.
 */
struct pty_session *pty_session_slot_at(uint32_t index);

/**
 * @brief Return the current addressable PTY session slot count.
 * @return Number of stable numeric PTY slots backed by allocated pages.
 */
uint32_t pty_session_slot_capacity(void);

int pty_get_node(uint32_t pty_id, struct storage_node *node);

int pty_lookup_path(const char *path, struct storage_node *node);

/** @brief Resolve one of the six fixed Linux virtual consoles.
 * @param path Device pathname, nullable.
 * @param node Optional output storage node.
 * @return Zero if resolved or negative ENOENT. */
int pty_lookup_vt_path(const char *path, struct storage_node *node);

int pty_inode_permissions(const struct storage_node *node,
                           struct reliefos_permissions *value, bool write);

/**
 * @brief Append length bytes into the ring, overwriting the oldest when full; returns bytes written.
 */
uint32_t ring_push(uint8_t *ring, uint32_t cap, uint32_t *head, uint32_t *tail,
                          const char *buffer, uint32_t length);

/**
 * @brief Remove up to length bytes from the ring into buffer; returns bytes read.
 */
uint32_t ring_pop(uint8_t *ring, uint32_t cap, uint32_t *head, uint32_t *tail,
                         char *buffer, uint32_t length);

/**
 * @brief Move the buffered canonical line into the input queue and clear it.
 */
void pty_commit_canonical_input(struct pty_session *session);

/**
 * @brief Reset allocated PTY sessions while preserving stable backing pages.
 */
void pty_init(void);

/** @brief Reserve tty1 through tty6 before user processes start.
 * @return Zero on success, negative ENOMEM if allocation fails. */
int pty_vt_init(void);

/** @brief Read the Linux VT ownership mode for a fixed console. */
int pty_vt_get_mode(uint32_t pty_id, struct vt_mode *mode);

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
int pty_vt_set_mode(uint32_t pty_id, const struct vt_mode *mode);

/**
 * @brief Return a fixed virtual console to its default Linux ownership state.
 * @param pty_id Fixed terminal identifier, one through six.
 * @return Zero on success, negative EINVAL for an invalid console.
 *
 * Controller recovery cancels the hand-off, restores KD_TEXT and the default
 * keyboard mode. Plain VT_SETMODE(VT_AUTO) does not reset KD or keyboard mode.
 */
int pty_vt_reset_mode(uint32_t pty_id);

/** @brief Read the Linux virtual-console keyboard translation mode. */
int pty_vt_get_keyboard_mode(uint32_t pty_id, int *mode);

/**
 * @brief Set the Linux virtual-console keyboard translation mode.
 * @param pty_id Fixed terminal identifier, one through six.
 * @param mode One of K_RAW, K_XLATE, K_MEDIUMRAW, K_UNICODE or K_OFF.
 * @return Zero on success, negative EINVAL for an invalid console or mode.
 */
int pty_vt_set_keyboard_mode(uint32_t pty_id, int mode);

/**
 * @brief Build the Linux VT_GETSTATE occupancy bitmap.
 * @return Bit zero for tty0 and bit n for each referenced console n.
 */
uint32_t pty_vt_state_bitmap(void);

/**
 * @brief Return the first virtual console available for allocation.
 * @return First unreferenced fixed console number, or -1 if none is available.
 */
int pty_vt_open_query(void);

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
int pty_vt_release_display(uint32_t pty_id, int request);

/** @brief Resolve a fixed VT after initialization.
 * @param number Virtual console number.
 * @return Terminal identifier, or zero for an invalid/uninitialized VT. */
uint32_t pty_vt_id(uint32_t number);

/** @brief Resolve a terminal to a fixed VT.
 * @param pty_id Terminal identifier.
 * @return VT number or zero for a dynamic PTY. */
uint32_t pty_vt_number(uint32_t pty_id);

/** @brief Read the selected virtual console.
 * @return One through six, or zero before initialization. */
uint32_t pty_vt_active(void);

/** @brief Read display invalidation state under the execution transaction.
 * @return Generation incremented by visible VT switches and mode activations.
 */
uint64_t pty_vt_generation(void);

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
int pty_vt_switch(uint32_t number);

/**
 * @brief Wait for a VT under the syscall execution lock; switching wakes all waiters.
 * @param number Requested fixed VT number, one through six.
 * @return Zero if active, negative EINVAL, or KERNEL_SYSCALL_BLOCKED for retry.
 */
int64_t pty_vt_wait_active(uint32_t number);

/** @brief Change display mode under the execution transaction.
 * @param pty_id Fixed terminal identifier.
 * @param enabled One for graphics, zero for text.
 * @return Zero on success or negative EINVAL. */
int pty_vt_set_graphics(uint32_t pty_id, int enabled);

/** @brief Inspect the selected display mode.
 * @return Nonzero if the active VT is graphical. */
int pty_vt_graphical_active(void);

/** @brief Inspect one fixed virtual console.
 * @param pty_id Fixed terminal identifier.
 * @return Nonzero if graphical, zero if text or invalid. */
int pty_vt_graphical(uint32_t pty_id);

/**
 * @brief Offer one physical keyboard event to the console terminal.
 * @param keycode Set-1 make/break code after 0xe0 extension normalization.
 * @param pressed Non-zero for a make code, zero for a break code.
 * @return Nothing.
 *
 * Keyboard translation is independent of KD display mode. OFF and raw modes
 * suppress cooked shortcuts; raw streams include modifiers and supported E0
 * extensions. The deferred input consumer still forwards the physical
 * Ctrl+Alt+Fn chord while a graphical evdev grab is active, so this handler
 * can complete a VT switch without leaking printable keys to the console.
 * It runs after VT initialization under the kernel execution transaction,
 * never from a keyboard ISR.
 */
void pty_console_key_event(uint8_t keycode, uint8_t pressed);

/**
 * @brief Allocate and initialize a PTY with default termios and window size.
 * @param owner_pid Task that opened ptmx; zero is reserved for kernel consoles.
 * @return Stable positive 1-based PTY ID or negative ENOMEM when storage growth fails.
 */
int32_t pty_create(uint32_t owner_pid);

/**
 * @brief Reclaim a hung-up PTY once no endpoint or controlling-TTY reference remains.
 */
void pty_reap_hungup(uint32_t pty_id);

int pty_transfer_get(uint32_t pty_id, uint32_t endpoint);

uint32_t pty_transfer_count(uint32_t pty_id, int master_only);

void pty_transfer_put(uint32_t pty_id, uint32_t endpoint);

/**
 * @brief Close the master side of pty_id using Unix98 hangup semantics.
 *
 * The session is not freed while slave descriptors remain open. The slave
 * keeps draining buffered input, then reports EOF/POLLHUP and rejects writes
 * with EIO. SIGHUP and SIGCONT are delivered to the controlling session leader.
 */
int pty_destroy(uint32_t owner_pid, uint32_t pty_id);

/**
 * @brief Return 1 if owner_pid owns pty_id.
 */
int pty_is_owner(uint32_t pty_id, uint32_t owner_pid);

/**
 * @brief Return 1 if pty_id is an allocated session.
 */
int pty_is_active(uint32_t pty_id);

int pty_is_hungup(uint32_t pty_id);

int pty_slave_open_allowed(uint32_t pty_id);

int pty_set_lock(uint32_t pty_id, int locked);

int pty_get_lock(uint32_t pty_id, int *locked);

/**
 * @brief Drain up to length bytes of terminal output for the owner; returns bytes read, or -22 if not owned.
 */
int64_t pty_read_output(uint32_t owner_pid, uint32_t pty_id, char *buffer, uint32_t length);

/**
 * @brief Process input bytes through termios (CR->NL, signals, line editing) and queue the results; returns bytes consumed.
 */
int64_t pty_write_input(uint32_t owner_pid, uint32_t pty_id, const char *buffer, uint32_t length);

/**
 * @brief Drain up to length bytes from the input queue; returns bytes read, or -5 if the session is invalid.
 */
int64_t pty_read_input(uint32_t pty_id, char *buffer, uint32_t length);

/**
 * @brief Return the number of unread input bytes.
 */
uint32_t pty_input_available(uint32_t pty_id);

uint32_t pty_output_available(uint32_t pty_id);

/**
 * @brief Queue up to length bytes of output for the reader; returns bytes written, or -5 if the session is invalid.
 */
int64_t pty_write_output(uint32_t pty_id, const char *buffer, uint32_t length);

/**
 * @brief Copy the session termios into *termios; returns 0, or -22 on a bad id or null pointer.
 */
int pty_get_termios(uint32_t pty_id, struct reliefos_pty_termios *termios);

void pty_flush_input(uint32_t pty_id);

int pty_set_termios(uint32_t pty_id, const struct reliefos_pty_termios *termios);

/**
 * @brief Copy the session window size into *winsize; returns 0, or -22 on a bad id or null pointer.
 */
int pty_get_winsize(uint32_t pty_id, struct reliefos_pty_winsize *winsize);

/**
 * @brief Preserve the platform row/column API and notify the foreground group.
 */
int pty_set_winsize(uint32_t pty_id, const struct reliefos_pty_winsize *winsize);

int pty_get_linux_winsize(uint32_t pty_id, struct linux_winsize *winsize);

int pty_set_linux_winsize(uint32_t pty_id, const struct linux_winsize *winsize);

/**
 * @brief Gets the process group currently receiving terminal-generated signals.
 * @param pty_id PTY identifier.
 * @param process_group Destination for the foreground process-group identifier.
 * @return Zero on success or a negative errno-style failure.
 */
int pty_get_foreground_pgid(uint32_t pty_id, uint32_t *process_group);

/**
 * @brief Changes the group that owns foreground terminal input.
 * @param pty_id PTY identifier.
 * @param caller_pid Attached process requesting the change.
 * @param process_group New foreground process-group identifier.
 * @return Zero on success or a negative errno-style failure.
 */
int pty_set_foreground_pgid(uint32_t pty_id, uint32_t caller_pid,
                            uint32_t process_group);

/**
 * @brief Apply Linux TIOCSCTTY session-leader, read access and steal rules.
 * @param pty_id Active terminal ID.
 * @param caller_pid Calling thread ID.
 * @param steal Raw ioctl argument, whose value 1 requests privileged stealing.
 * @param readable Whether the descriptor was opened for reading.
 * @return Zero on attachment or a negative Linux errno.
 */
int pty_acquire_controlling(uint32_t pty_id, uint32_t caller_pid, int steal, int readable);

int pty_get_session(uint32_t pty_id, uint32_t *session_id);

void pty_open_controlling(uint32_t pty_id, uint32_t caller_pid, int readable);

int pty_detach_controlling(uint32_t pty_id, uint32_t caller_pid);

void pty_process_session_exit(uint32_t tgid);

int64_t pty_check_change(uint32_t pty_id, uint32_t caller_pid, int signal_number);

/**
 * @brief Retire legacy owners without closing a retained Unix98 master.
 * @param pid Task whose legacy master ownership is being retired.
 */
void pty_process_exit(uint32_t pid);

#endif

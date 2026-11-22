#include "internal.h"
#include <linux/errno.h>
#include <linux/poll.h>

enum { PTY_INPUT_LINE = 1, PTY_INPUT_EOF = 2 };

static uint32_t pty_ring_used(uint32_t head, uint32_t tail, uint32_t capacity)
{ return head >= tail ? head - tail : capacity - tail + head; }

static uint32_t pty_queue_input(struct pty_session *session, const char *bytes,
                                uint32_t length, uint8_t end)
{
    uint32_t first = session->input_head;
    uint32_t count = ring_push(session->input, PTY_INPUT_CAP,
        &session->input_head, &session->input_tail, bytes, length);
    for (uint32_t i = 0; i < count; ++i)
        session->input_end[(first + i) % PTY_INPUT_CAP] =
            i + 1 == length ? end : 0;
    return count;
}

/** @brief Queue terminal query traffic without turning it into keyboard echo. */
void pty_vt_reply(uint32_t number, const char *bytes, size_t len)
{
    struct pty_session *session=find_session(number);
    if (number<1 || number>VT_COUNT || !session || !session->console || session->hungup || !bytes || !len) return;
    if (len>64) len=64;
    if (PTY_INPUT_CAP - 1 - pty_ring_used(session->input_head, session->input_tail,
                                         PTY_INPUT_CAP) < len) return;
    if (pty_queue_input(session, bytes, (uint32_t)len, PTY_INPUT_LINE))
        kernel_poll_readiness_notify();
}

/**
 * @brief Return 1 if the session is in ICANON line mode.
 */
static int pty_canonical_mode(const struct pty_session *session);

/**
 * @brief Expand one slave output byte through OPOST processing into out; returns bytes emitted.
 *
 * Mirrors the Linux n_tty output rules for the flags that rewrite bytes:
 * ONLCR, OCRNL, ONOCR, ONLRET, OLCUC and TAB3 (XTABS). The tracked cursor
 * column feeds ONOCR/ONLRET/TAB3; without OPOST the byte passes through
 * untouched and the column stays frozen, as on Linux.
 *
 * @param session PTY session whose oflag and column state apply.
 * @param byte Output byte as written by the slave.
 * @param out Destination of at most 8 translated bytes.
 * @return Number of bytes placed in out; zero when the byte is suppressed.
 */
static uint32_t pty_output_post_process(struct pty_session *session, char byte, char out[8]);

static uint32_t pty_output_room(const struct pty_session *session)
{
    return PTY_OUTPUT_CAP - 1 - pty_ring_used(session->output_head,
                                            session->output_tail, PTY_OUTPUT_CAP);
}

/* Echo uses the same output processing as slave writes. While IXON has
 * stopped output, retain the echo until VSTART instead of losing edits. */
static bool pty_echo_room(const struct pty_session *session, uint32_t length)
{
    return (session->console && !session->output_stopped) ||
           pty_output_room(session) >= length;
}

static void pty_echo_emit(struct pty_session *session, uint32_t id,
                           const char *bytes, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        char out[8];
        uint32_t count = pty_output_post_process(session, bytes[i], out);
        if (session->console && !session->output_stopped)
            console_vt_write(id, out, count);
        else
            (void)ring_push(session->output, PTY_OUTPUT_CAP, &session->output_head,
                            &session->output_tail, out, count);
    }
    kernel_poll_readiness_notify();
}

static bool pty_echo_byte(struct pty_session *session, uint32_t id, uint8_t byte)
{
    if (!(session->termios.c_lflag & LINUX_ECHO) &&
        !(byte == '\n' && pty_canonical_mode(session) &&
          (session->termios.c_lflag & LINUX_ECHONL))) return true;
    char bytes[2] = {(char)byte, 0};
    uint32_t length = 1;
    if ((session->termios.c_lflag & LINUX_ECHOCTL) &&
        ((byte < 32 && byte != '\t' && byte != '\n') || byte == 127)) {
        bytes[0] = '^'; bytes[1] = byte == 127 ? '?' : (char)(byte + '@');
        length = 2;
    }
    uint32_t column = session->output_column, needed = 0;
    for (uint32_t i = 0; i < length; ++i) {
        char out[8];
        needed += pty_output_post_process(session, bytes[i], out);
    }
    session->output_column = column;
    if (!pty_echo_room(session, needed)) return false;
    pty_echo_emit(session, id, bytes, length);
    return true;
}

static uint32_t pty_previous_character(const struct pty_session *session, uint32_t end)
{
    if (!end) return 0;
    --end;
    if (session->termios.c_iflag & LINUX_IUTF8)
        while (end && (session->canonical_input[end] & 0xc0) == 0x80) --end;
    return end;
}

/* Erase only cells belonging to the pending canonical line. In particular,
 * an empty-line Backspace or Ctrl-U must never damage the shell prompt. */
static bool pty_echo_erase(struct pty_session *session, uint32_t id,
                            uint32_t target, uint8_t byte, bool kill)
{
    uint32_t flags = session->termios.c_lflag;
    if (!(flags & LINUX_ECHO) || target == session->canonical_length) return true;
    if (!(flags & LINUX_ECHOE) || (kill && !(flags & LINUX_ECHOKE))) {
        if (!pty_echo_room(session, 4)) return false;
        (void)pty_echo_byte(session, id, byte);
        if (kill && (flags & LINUX_ECHOK)) pty_echo_emit(session, id, "\n", 1);
        return true;
    }
    uint32_t needed = 0;
    for (uint32_t i = target; i < session->canonical_length; ++i) {
        uint8_t width = session->canonical_width[i];
        needed += (width & 0x7f) * (width & 0x80 ? 1u : 3u);
    }
    if (!pty_echo_room(session, needed)) return false;
    for (uint32_t end = session->canonical_length; end > target;) {
        end = pty_previous_character(session, end);
        uint8_t width = session->canonical_width[end];
        for (uint32_t i = 0; i < (width & 0x7f); ++i)
            pty_echo_emit(session, id, width & 0x80 ? "\b" : "\b \b", width & 0x80 ? 1 : 3);
    }
    return true;
}

/**
 * @brief Replace the session termios, first flushing any pending canonical line when leaving ICANON; returns 0 or -22.
 */
static uint32_t pty_baud_rate(uint32_t encoding, uint32_t custom);


/**
 * @brief Move the buffered canonical line into the input queue and clear it.
 */
void pty_commit_canonical_input(struct pty_session *session)
{
    if (!session || !session->canonical_length) {
        return;
    }
    uint32_t count = pty_queue_input(session, (const char *)session->canonical_input,
                                      session->canonical_length, PTY_INPUT_LINE);
    session->canonical_length -= count;
    __builtin_memmove(session->canonical_input, session->canonical_input + count,
                       session->canonical_length);
    __builtin_memmove(session->canonical_width, session->canonical_width + count,
                       session->canonical_length);
}

/**
 * @brief Return 1 if the session is in ICANON line mode.
 */
static int pty_canonical_mode(const struct pty_session *session)
{
    return session &&
           (session->termios.c_lflag & RELIEFOS_PTY_LFLAG_ICANON) != 0;
}

int pty_set_packet_mode(uint32_t pty_id, int enabled)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || session->console) return -LINUX_ENOTTY;
    if (session->packet_mode != (enabled != 0)) {
        session->packet_status = 0;
        session->packet_mode = enabled != 0;
    }
    return 0;
}

int pty_get_packet_mode(uint32_t pty_id, int *enabled)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || session->console) return -LINUX_ENOTTY;
    *enabled = session->packet_mode;
    return 0;
}

static void pty_packet_event(struct pty_session *session, uint8_t set, uint8_t clear)
{
    if (!session->packet_mode) return;
    session->packet_status = (session->packet_status & ~clear) | set;
    kernel_poll_readiness_notify();
}

short pty_poll_endpoint(uint32_t pty_id, bool master)
{
    struct pty_session *session = find_session(pty_id);
    if (!session) return POLLHUP | POLLERR;
    if (session->hungup) return POLLIN | POLLRDNORM | POLLHUP | (master ? 0 : POLLERR);
    short ready = 0;
    if (master ? !session->output_stopped && session->output_head != session->output_tail :
                 session->input_head != session->input_tail)
        ready |= POLLIN | POLLRDNORM;
    if (master && session->packet_mode && session->packet_status)
        ready |= POLLIN | POLLRDNORM | POLLPRI;
    uint32_t room = master ? PTY_INPUT_CAP - 1 -
        pty_ring_used(session->input_head, session->input_tail, PTY_INPUT_CAP) :
        PTY_OUTPUT_CAP - 1 - pty_ring_used(session->output_head, session->output_tail, PTY_OUTPUT_CAP);
    uint32_t input_needed = pty_canonical_mode(session) ? session->canonical_length + 1 : 1;
    if (master ? room >= input_needed : !session->output_stopped && (session->console || room >= 8))
        ready |= POLLOUT | POLLWRNORM;
    return ready;
}

/**
 * @brief Drain up to length bytes of terminal output for the owner; returns bytes read, or -22 if not owned.
 */
int64_t pty_read_output(uint32_t owner_pid, uint32_t pty_id, char *buffer,
                        uint32_t length, enum pty_read_phase *phase)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || (owner_pid && session->owner_pid != owner_pid)) {
        return -LINUX_EINVAL;
    }
    if (!buffer || !length || (phase && *phase == PTY_READ_CONTROL)) return 0;
    bool first = !phase || *phase == PTY_READ_FIRST;
    if (first && session->packet_mode && session->packet_status) {
        *buffer = (char)session->packet_status;
        session->packet_status = 0;
        if (phase) *phase = PTY_READ_CONTROL;
        return 1;
    }
    if (session->output_head == session->output_tail || session->output_stopped)
        return !first || session->hungup ? 0 : -LINUX_EAGAIN;
    uint32_t prefix = 0;
    if (session->packet_mode) {
        if (first) {
            *buffer = TIOCPKT_DATA;
            prefix = 1;
        }
        if (phase) *phase = PTY_READ_DATA;
    }
    uint32_t count = ring_pop(session->output, PTY_OUTPUT_CAP,
                             &session->output_head, &session->output_tail,
                             buffer + prefix, length - prefix);
    if (count) kernel_poll_readiness_notify();
    return prefix + count;
}

static void pty_stop_output(struct pty_session *session, uint32_t id, bool stopped)
{
    if (session->output_stopped == stopped) return;
    session->output_stopped = stopped;
    if (!stopped && session->console) {
        char bytes[128];
        uint32_t count;
        /* Console echoes queued during XOFF are already post-processed. */
        while ((count = ring_pop(session->output, PTY_OUTPUT_CAP,
                &session->output_head, &session->output_tail, bytes, sizeof(bytes))))
            console_vt_write(id, bytes, count);
    }
    if (session->packet_mode) {
        pty_packet_event(session, stopped ? TIOCPKT_STOP : TIOCPKT_START,
                         stopped ? TIOCPKT_START : TIOCPKT_STOP);
    } else if (!stopped) {
        kernel_poll_readiness_notify();
    }
}

static void pty_clear_input(struct pty_session *session)
{
    session->input_tail = session->input_head;
    session->canonical_length = 0;
    session->literal_next = false;
    kernel_poll_readiness_notify();
}

/**
 * @brief Process input bytes through termios (CR->NL, signals, line editing) and queue the results; returns bytes consumed.
 */
int64_t pty_write_input(uint32_t owner_pid, uint32_t pty_id, const char *buffer, uint32_t length)
{
    struct pty_session *session = find_session(pty_id);
    uint32_t input_before = session ? session->input_head : 0;
    uint32_t written = 0;
    if (!session || (owner_pid && session->owner_pid != owner_pid)) {
        return -22;
    }
    if (session->hungup) {
        return -5;
    }
    if (!buffer || length == 0) {
        return 0;
    }
    if (!session->console && !session->input_reported) {
        session->input_reported = 1;
        console_debug("[reliefnt] pty=%u first master input owner=%u len=%u\n",
                       pty_id, session->owner_pid, length);
    }
    for (uint32_t i = 0; i < length; ++i) {
        uint8_t input = (uint8_t)buffer[i];
        bool literal = session->literal_next;
        if (session->termios.c_iflag & LINUX_ISTRIP) input &= 0x7f;
        if (!literal && (session->termios.c_iflag & LINUX_IXON)) {
            if (input && input == session->termios.c_cc[RELIEFOS_PTY_CC_VSTART]) {
                pty_stop_output(session, pty_id, false);
                ++written;
                continue;
            }
            if (input && input == session->termios.c_cc[RELIEFOS_PTY_CC_VSTOP]) {
                pty_stop_output(session, pty_id, true);
                ++written;
                continue;
            }
            if (session->termios.c_iflag & LINUX_IXANY) pty_stop_output(session, pty_id, false);
        }
        if (input == '\r' && (session->termios.c_iflag & LINUX_IGNCR)) {
            ++written; continue;
        }
        if (input == '\r' &&
            (session->termios.c_iflag & RELIEFOS_PTY_IFLAG_ICRNL)) {
            input = '\n';
        } else if (input == '\n' && (session->termios.c_iflag & LINUX_INLCR)) input = '\r';
        if (!literal && input && (session->termios.c_lflag & RELIEFOS_PTY_LFLAG_ISIG)) {
            int signal = input == session->termios.c_cc[RELIEFOS_PTY_CC_VINTR] ? 2 :
                         input == session->termios.c_cc[RELIEFOS_PTY_CC_VSUSP] ? 20 :
                         input == session->termios.c_cc[RELIEFOS_PTY_CC_VQUIT] ? 3 : 0;
            if (signal) {
                if (!pty_echo_byte(session, pty_id, input)) break;
                if (!(session->termios.c_lflag & LINUX_NOFLSH)) {
                    pty_clear_input(session);
                    pty_packet_event(session, TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE, 0);
                }
                if (session->termios.c_iflag & LINUX_IXON) pty_stop_output(session, pty_id, false);
                if (session->foreground_pgid)
                    (void)sched_signal_kernel_group(session->foreground_pgid, signal);
                ++written;
                continue;
            }
        }
        if (!pty_canonical_mode(session)) {
            /* A raw-mode transition may still have older cooked bytes waiting
             * for ring space. Keep those ahead of newly received input. */
            pty_commit_canonical_input(session);
            if (session->canonical_length ||
                pty_ring_used(session->input_head, session->input_tail, PTY_INPUT_CAP) == PTY_INPUT_CAP - 1) break;
            if (!pty_echo_byte(session, pty_id, input)) break;
            if (!pty_queue_input(session, (const char *)&input, 1, 0)) break;
            session->literal_next = false;
            ++written;
            continue;
        }

        bool extended = (session->termios.c_lflag & LINUX_IEXTEN) != 0;
        bool erase = !literal && input && input == session->termios.c_cc[LINUX_VERASE];
        bool kill = !literal && input && input == session->termios.c_cc[LINUX_VKILL];
        bool word = !literal && extended && input && input == session->termios.c_cc[LINUX_VWERASE];
        if (erase || kill || word) {
            uint32_t target = session->canonical_length;
            if (erase) target = pty_previous_character(session, target);
            else if (kill) target = 0;
            else {
                while (target && (session->canonical_input[target - 1] == ' ' ||
                                   session->canonical_input[target - 1] == '\t')) --target;
                while (target && session->canonical_input[target - 1] != ' ' &&
                                   session->canonical_input[target - 1] != '\t')
                    target = pty_previous_character(session, target);
            }
            if (!pty_echo_erase(session, pty_id, target, input, kill)) break;
            session->canonical_length = target;
        } else if (!literal && extended && input && input == session->termios.c_cc[LINUX_VLNEXT]) {
            if ((session->termios.c_lflag & (LINUX_ECHO | LINUX_ECHOCTL)) ==
                (LINUX_ECHO | LINUX_ECHOCTL)) {
                if (!pty_echo_room(session, 2)) break;
                pty_echo_emit(session, pty_id, "^\b", 2);
            }
            session->literal_next = true;
        } else if (!literal && input && input == session->termios.c_cc[RELIEFOS_PTY_CC_VEOF]) {
            /* VEOF is a delimiter, never a byte delivered to the reader. */
            uint32_t required = session->canonical_length ? session->canonical_length : 1;
            if (PTY_INPUT_CAP - 1 - pty_ring_used(session->input_head, session->input_tail,
                                                 PTY_INPUT_CAP) < required) break;
            if (session->canonical_length) pty_commit_canonical_input(session);
            else { char eof = 0; (void)pty_queue_input(session, &eof, 1, PTY_INPUT_EOF); }
        } else {
            bool end = !literal && (input == '\n' || (input &&
                (input == session->termios.c_cc[RELIEFOS_PTY_CC_VEOL] ||
                 (extended && input == session->termios.c_cc[LINUX_VEOL2]))));
            if (end && PTY_INPUT_CAP - 1 - pty_ring_used(session->input_head,
                  session->input_tail, PTY_INPUT_CAP) < session->canonical_length + 1) break;
            /* Reserve the last slot for a delimiter, so an overlong line can
             * always be submitted and cannot deadlock the terminal. */
            if (end || session->canonical_length < PTY_INPUT_CAP - 2) {
                uint32_t column = session->output_column;
                if (!pty_echo_byte(session, pty_id, input)) break;
                uint32_t width = session->output_column >= column ? session->output_column - column : 0;
                if (width > 127) width = 127;
                session->canonical_width[session->canonical_length] = (uint8_t)width |
                    (input == '\t' ? 0x80 : 0);
                session->canonical_input[session->canonical_length++] = (uint8_t)input;
            }
            session->literal_next = false;
            if (end) {
                pty_commit_canonical_input(session);
            }
        }
        ++written;
    }
    if (session->input_head != input_before)
        kernel_poll_readiness_notify();
    return written ? (int64_t)written : -LINUX_EAGAIN;
}

/**
 * @brief Drain up to length bytes from the input queue; returns bytes read, or -5 if the session is invalid.
 */
int64_t pty_read_input(uint32_t pty_id, char *buffer, uint32_t length)
{
    struct pty_session *session = find_session(pty_id);
    if (!session) {
        return -5;
    }
    if (!buffer || length == 0) {
        return 0;
    }
    if (session->canonical_length && (!pty_canonical_mode(session) || session->hungup))
        pty_commit_canonical_input(session);
    uint32_t queued = 0;
    bool delimiter = false;
    while (queued < length && session->input_tail != session->input_head) {
        uint32_t at = session->input_tail;
        uint8_t end = session->input_end[at];
        session->input_tail = (at + 1) % PTY_INPUT_CAP;
        if (!(end & PTY_INPUT_EOF)) buffer[queued++] = (char)session->input[at];
        if (pty_canonical_mode(session) && end) { delimiter = true; break; }
    }
    if (queued || delimiter) kernel_poll_readiness_notify();
    if (queued || delimiter || session->hungup) return queued;
    /* An empty live terminal is not EOF. The syscall dispatcher parks a
     * blocking reader on EAGAIN, or exposes it for O_NONBLOCK descriptors. */
    if (!pty_canonical_mode(session) &&
        session->termios.c_cc[RELIEFOS_PTY_CC_VMIN] == 0 &&
        session->termios.c_cc[RELIEFOS_PTY_CC_VTIME] == 0) {
        return 0;
    }
    return -11;
}

/**
 * @brief Return the number of unread input bytes.
 */
uint32_t pty_input_available(uint32_t pty_id)
{
    struct pty_session *session = find_session(pty_id);
    if (!session) {
        return 0;
    }
    uint32_t bytes = 0;
    for (uint32_t at = session->input_tail; at != session->input_head;
         at = (at + 1) % PTY_INPUT_CAP)
        if (!(session->input_end[at] & PTY_INPUT_EOF)) ++bytes;
    return bytes;
}

bool pty_input_ready(uint32_t pty_id)
{
    struct pty_session *session = find_session(pty_id);
    return session && session->input_head != session->input_tail;
}

uint32_t pty_output_available(uint32_t pty_id)
{
    struct pty_session *session = find_session(pty_id);
    if (!session) return 0;
    if (session->output_head >= session->output_tail) {
        return session->output_head - session->output_tail;
    }
    return PTY_OUTPUT_CAP - session->output_tail + session->output_head;
}

/**
 * @brief Expand one slave output byte through OPOST processing into out; returns bytes emitted.
 *
 * Mirrors the Linux n_tty output rules for the flags that rewrite bytes:
 * ONLCR, OCRNL, ONOCR, ONLRET, OLCUC and TAB3 (XTABS). The tracked cursor
 * column feeds ONOCR/ONLRET/TAB3; without OPOST the byte passes through
 * untouched and the column stays frozen, as on Linux.
 *
 * @param session PTY session whose oflag and column state apply.
 * @param byte Output byte as written by the slave.
 * @param out Destination of at most 8 translated bytes.
 * @return Number of bytes placed in out; zero when the byte is suppressed.
 */
static uint32_t pty_output_post_process(struct pty_session *session, char byte, char out[8])
{
    uint32_t oflag = session->termios.c_oflag;
    uint32_t column = session->output_column;
    uint32_t count = 0;

    if ((oflag & LINUX_OPOST) == 0) {
        out[0] = byte;
        return 1;
    }
    switch (byte) {
    case '\n':
        if (oflag & LINUX_ONLCR) {
            out[count++] = '\r';
            column = 0;
        }
        out[count++] = '\n';
        if (oflag & LINUX_ONLRET) {
            column = 0;
        }
        break;
    case '\r':
        if ((oflag & LINUX_ONOCR) && column == 0) {
            break;
        }
        if (oflag & LINUX_OCRNL) {
            out[count++] = '\n';
            if (oflag & LINUX_ONLRET) {
                column = 0;
            }
        } else {
            out[count++] = '\r';
            column = 0;
        }
        break;
    case '\t':
        if ((oflag & LINUX_TABDLY) == LINUX_TAB3) {
            uint32_t spaces = 8u - (column & 7u);
            while (spaces--) {
                out[count++] = ' ';
                ++column;
            }
        } else {
            out[count++] = '\t';
            column += 8u - (column & 7u);
        }
        break;
    case '\b':
        if (column) {
            --column;
        }
        out[count++] = '\b';
        break;
    default:
        if ((oflag & LINUX_OLCUC) && byte >= 'a' && byte <= 'z') {
            byte = (char)(byte - 'a' + 'A');
        }
        out[count++] = byte;
        if ((uint8_t)byte >= 0x20u && byte != 0x7fu &&
            !((session->termios.c_iflag & LINUX_IUTF8) && ((uint8_t)byte & 0xc0) == 0x80)) {
            ++column;
        }
        break;
    }
    session->output_column = column;
    return count;
}

/**
 * @brief Queue up to length bytes of output for the reader; returns bytes written, or -5 if the session is invalid.
 */
int64_t pty_write_output(uint32_t pty_id, const char *buffer, uint32_t length)
{
    struct pty_session *session = find_session(pty_id);
    uint32_t output_before = pty_output_available(pty_id);
    char staged[512];
    uint32_t staged_length = 0;
    uint32_t written = 0;

    if (!session) {
        return -5;
    }
    if (session->hungup) {
        return -5; /* EIO */
    }
    if (!buffer || length == 0) {
        return 0;
    }
    if (session->output_stopped) return -LINUX_EAGAIN;
    if (!session->console && !session->output_reported) {
        session->output_reported = 1;
        console_debug("[reliefnt] pty=%u first slave output len=%u\n",
                       pty_id, length);
    }
    for (uint32_t i = 0; i < length; ++i) {
        char out[8];
        uint32_t column = session->output_column;
        uint32_t count = pty_output_post_process(session, buffer[i], out);
        if (!session->console && PTY_OUTPUT_CAP - 1 - pty_output_available(pty_id) - staged_length < count) {
            session->output_column = column;
            break;
        }
        for (uint32_t j = 0; j < count; ++j) {
            if (staged_length == sizeof(staged)) {
                if (session->console) {
                    console_vt_write(pty_id, staged, staged_length);
                } else {
                    (void)ring_push(session->output, PTY_OUTPUT_CAP,
                                    &session->output_head, &session->output_tail,
                                    staged, staged_length);
                }
                staged_length = 0;
            }
            staged[staged_length++] = out[j];
        }
        ++written;
    }
    if (staged_length) {
        if (session->console) {
            console_vt_write(pty_id, staged, staged_length);
        } else {
            (void)ring_push(session->output, PTY_OUTPUT_CAP,
                            &session->output_head, &session->output_tail,
                            staged, staged_length);
        }
    }
    if (pty_output_available(pty_id) != output_before)
        kernel_poll_readiness_notify();
    return written ? (int64_t)written : -LINUX_EAGAIN;
}

/**
 * @brief Copy the session termios into *termios; returns 0, or -22 on a bad id or null pointer.
 */
int pty_get_termios(uint32_t pty_id, struct reliefos_pty_termios *termios)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || !termios) {
        return -22;
    }
    *termios = session->termios;
    return 0;
}

/**
 * @brief Replace the session termios, first flushing any pending canonical line when leaving ICANON; returns 0 or -22.
 */
static uint32_t pty_baud_rate(uint32_t encoding, uint32_t custom)
{
    static const uint32_t rates[] = {
        0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400,
        4800, 9600, 19200, 38400, 0, 57600, 115200, 230400, 460800,
        500000, 576000, 921600, 1000000, 1152000, 1500000, 2000000,
        2500000, 3000000, 3500000, 4000000,
    };
    if (encoding == LINUX_BOTHER) return custom;
    return rates[(encoding & 0xfU) | ((encoding & LINUX_BOTHER) ? 16U : 0U)];
}

void pty_flush_input(uint32_t pty_id)
{
    struct pty_session *session = find_session(pty_id);
    if (!session) return;
    pty_clear_input(session);
    pty_packet_event(session, TIOCPKT_FLUSHREAD, 0);
    kernel_poll_readiness_notify();
}

int pty_flush(uint32_t pty_id, bool master, uint64_t queue)
{
    struct pty_session *session = find_session(pty_id);
    if (!session) return -LINUX_ENOTTY;
    if (queue > LINUX_TCIOFLUSH) return -LINUX_EINVAL;
    if (master) {
        if (queue != LINUX_TCOFLUSH) session->output_tail = session->output_head;
    } else {
        if (queue != LINUX_TCOFLUSH) pty_clear_input(session);
        /* Writes enter the peer's receive queue synchronously. Bytes already
         * received by the master survive a slave output flush, as on Linux. */
        uint8_t status = queue == LINUX_TCIFLUSH ? TIOCPKT_FLUSHREAD :
                         queue == LINUX_TCOFLUSH ? TIOCPKT_FLUSHWRITE :
                         TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE;
        pty_packet_event(session, status, 0);
    }
    kernel_poll_readiness_notify();
    return 0;
}

static bool pty_default_flow(const struct reliefos_pty_termios *termios)
{
    return (termios->c_iflag & LINUX_IXON) &&
           termios->c_cc[RELIEFOS_PTY_CC_VSTART] == 17 &&
           termios->c_cc[RELIEFOS_PTY_CC_VSTOP] == 19;
}

int pty_set_termios(uint32_t pty_id, const struct reliefos_pty_termios *termios)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || !termios) {
        return -22;
    }
    /**
 * @brief A program switching to raw mode must be able to read a line that was already typed in cooked mode instead of leaving it stranded forever.
 */
    if (pty_canonical_mode(session) &&
        (termios->c_lflag & RELIEFOS_PTY_LFLAG_ICANON) == 0) {
        pty_commit_canonical_input(session);
    }
    if ((session->termios.c_lflag ^ termios->c_lflag) & LINUX_ICANON)
        session->literal_next = false;
    uint8_t status = ((session->termios.c_lflag | termios->c_lflag) & LINUX_EXTPROC)
                         ? TIOCPKT_IOCTL : 0;
    uint8_t clear = 0;
    bool default_flow = pty_default_flow(termios);
    if (pty_default_flow(&session->termios) != default_flow) {
        clear = TIOCPKT_DOSTOP | TIOCPKT_NOSTOP;
        status |= default_flow ? TIOCPKT_DOSTOP : TIOCPKT_NOSTOP;
    }
    session->termios = *termios;
    uint32_t output = termios->c_cflag & LINUX_CBAUD;
    uint32_t input = (termios->c_cflag & LINUX_CIBAUD) >> LINUX_IBSHIFT;
    session->termios.c_ospeed = pty_baud_rate(output, termios->c_ospeed);
    session->termios.c_ispeed = input ? pty_baud_rate(input, termios->c_ispeed)
                                     : session->termios.c_ospeed;
    if (!(termios->c_iflag & LINUX_IXON)) pty_stop_output(session, pty_id, false);
    if (status) pty_packet_event(session, status, clear);
    kernel_poll_readiness_notify();
    return 0;
}

/**
 * @brief Copy the session window size into *winsize; returns 0, or -22 on a bad id or null pointer.
 */
int pty_get_winsize(uint32_t pty_id, struct reliefos_pty_winsize *winsize)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || !winsize) {
        return -22;
    }
    winsize->ws_row = session->winsize.ws_row;
    winsize->ws_col = session->winsize.ws_col;
    return 0;
}

/**
 * @brief Preserve the platform row/column API and notify the foreground group.
 */
int pty_set_winsize(uint32_t pty_id, const struct reliefos_pty_winsize *winsize)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || !winsize) {
        return -22;
    }
    struct linux_winsize native = session->winsize;
    native.ws_row = winsize->ws_row;
    native.ws_col = winsize->ws_col;
    return pty_set_linux_winsize(pty_id, &native);
}

int pty_get_linux_winsize(uint32_t pty_id, struct linux_winsize *winsize)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || !winsize) return -22;
    *winsize = session->winsize;
    return 0;
}

int pty_set_linux_winsize(uint32_t pty_id, const struct linux_winsize *winsize)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || !winsize) return -22;
    if (__builtin_memcmp(&session->winsize, winsize, sizeof(*winsize)) == 0) return 0;
    session->winsize = *winsize;
    if (session->foreground_pgid) sched_signal_kernel_group(session->foreground_pgid, 28);
    return 0;
}

#include "internal.h"

/** @brief Queue terminal query traffic without turning it into keyboard echo. */
void pty_vt_reply(uint32_t number, const char *bytes, size_t len)
{
    struct pty_session *session=find_session(number);
    if (number<1 || number>VT_COUNT || !session || !session->console || session->hungup || !bytes || !len) return;
    if (len>64) len=64;
    if (ring_push(session->input,PTY_INPUT_CAP,&session->input_head,&session->input_tail,bytes,(uint32_t)len))
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
    (void)ring_push(session->input, PTY_INPUT_CAP,
                    &session->input_head, &session->input_tail,
                    (const char *)session->canonical_input,
                    session->canonical_length);
    session->canonical_length = 0;
}

/**
 * @brief Return 1 if the session is in ICANON line mode.
 */
static int pty_canonical_mode(const struct pty_session *session)
{
    return session &&
           (session->termios.c_lflag & RELIEFOS_PTY_LFLAG_ICANON) != 0;
}

/**
 * @brief Drain up to length bytes of terminal output for the owner; returns bytes read, or -22 if not owned.
 */
int64_t pty_read_output(uint32_t owner_pid, uint32_t pty_id, char *buffer, uint32_t length)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || (owner_pid && session->owner_pid != owner_pid)) {
        return -22;
    }
    if (!buffer || length == 0) {
        return 0;
    }
    {
        int64_t queued = (int64_t)ring_pop(session->output, PTY_OUTPUT_CAP,
                                           &session->output_head,
                                           &session->output_tail,
                                           buffer, length);
        if (queued != 0 || !session->hungup) {
            return queued;
        }
    }
    return 0;
}

/**
 * @brief Process input bytes through termios (CR->NL, signals, line editing) and queue the results; returns bytes consumed.
 */
int64_t pty_write_input(uint32_t owner_pid, uint32_t pty_id, const char *buffer, uint32_t length)
{
    struct pty_session *session = find_session(pty_id);
    uint32_t input_before = pty_input_available(pty_id);
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
        char input = buffer[i];
        if (input == '\r' &&
            (session->termios.c_iflag & RELIEFOS_PTY_IFLAG_ICRNL)) {
            input = '\n';
        }
        if ((session->termios.c_lflag & RELIEFOS_PTY_LFLAG_ISIG) != 0 &&
            input == (char)session->termios.c_cc[RELIEFOS_PTY_CC_VINTR]) {
            session->canonical_length = 0;
            if (session->foreground_pgid) {
                (void)sched_signal_kernel_group(session->foreground_pgid, 2);
            }
            ++written;
            continue;
        }
        if ((session->termios.c_lflag & RELIEFOS_PTY_LFLAG_ISIG) != 0 &&
            input == (char)session->termios.c_cc[RELIEFOS_PTY_CC_VSUSP]) {
            session->canonical_length = 0;
            if (session->foreground_pgid) {
                (void)sched_signal_kernel_group(session->foreground_pgid, 20);
            }
            ++written;
            continue;
        }
        if ((session->termios.c_lflag & RELIEFOS_PTY_LFLAG_ISIG) != 0 &&
            input == (char)session->termios.c_cc[RELIEFOS_PTY_CC_VQUIT]) {
            session->canonical_length = 0;
            if (session->foreground_pgid) {
                (void)sched_signal_kernel_group(session->foreground_pgid, 3);
            }
            ++written;
            continue;
        }
        if (!pty_canonical_mode(session)) {
            written += ring_push(session->input, PTY_INPUT_CAP,
                                 &session->input_head, &session->input_tail,
                                 &input, 1);
            continue;
        }

        if (input == (char)session->termios.c_cc[RELIEFOS_PTY_CC_VERASE]) {
            if (session->canonical_length) {
                --session->canonical_length;
            }
        } else if (input == (char)session->termios.c_cc[RELIEFOS_PTY_CC_VKILL]) {
            session->canonical_length = 0;
        } else if (input == (char)session->termios.c_cc[RELIEFOS_PTY_CC_VEOF]) {
            /**
 * @brief VEOF is a delimiter, not a byte delivered to the reader.
 */
            pty_commit_canonical_input(session);
        } else {
            if (session->canonical_length + 1U < PTY_INPUT_CAP) {
                session->canonical_input[session->canonical_length++] = (uint8_t)input;
            }
            if (input == '\n' ||
                (session->termios.c_cc[RELIEFOS_PTY_CC_VEOL] != 0 &&
                 input == (char)session->termios.c_cc[RELIEFOS_PTY_CC_VEOL])) {
                pty_commit_canonical_input(session);
            }
        }
        ++written;
    }
    if (pty_input_available(pty_id) != input_before)
        kernel_poll_readiness_notify();
    return (int64_t)written;
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
    {
        int64_t queued = (int64_t)ring_pop(session->input, PTY_INPUT_CAP,
                                           &session->input_head,
                                           &session->input_tail,
                                           buffer, length);
        if (queued != 0 || session->hungup) {
            return queued;
        }
    }
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
    if (session->input_head >= session->input_tail) {
        return session->input_head - session->input_tail;
    }
    return PTY_INPUT_CAP - session->input_tail + session->input_head;
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
        if ((uint8_t)byte >= 0x20u && byte != 0x7fu) {
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

    if (!session) {
        return -5;
    }
    if (session->hungup) {
        return -5; /* EIO */
    }
    if (!buffer || length == 0) {
        return 0;
    }
    if (!session->console && !session->output_reported) {
        session->output_reported = 1;
        console_debug("[reliefnt] pty=%u first slave output len=%u\n",
                       pty_id, length);
    }
    for (uint32_t i = 0; i < length; ++i) {
        char out[8];
        uint32_t count = pty_output_post_process(session, buffer[i], out);
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
    return (int64_t)length;
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
    session->input_tail = session->input_head;
    session->canonical_length = 0;
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
    session->termios = *termios;
    uint32_t output = termios->c_cflag & LINUX_CBAUD;
    uint32_t input = (termios->c_cflag & LINUX_CIBAUD) >> LINUX_IBSHIFT;
    session->termios.c_ospeed = pty_baud_rate(output, termios->c_ospeed);
    session->termios.c_ispeed = input ? pty_baud_rate(input, termios->c_ispeed)
                                     : session->termios.c_ospeed;
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

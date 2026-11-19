#ifndef RELIEFOS_UAPI_LINUX_TTY_H
#define RELIEFOS_UAPI_LINUX_TTY_H

#include <stdint.h>

struct linux_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

#define TIOCGWINSZ 0x5413UL
#define TIOCSWINSZ 0x5414UL
#define TIOCSCTTY  0x540eUL
#define TIOCGPGRP  0x540fUL
#define TIOCSPGRP  0x5410UL
#define TIOCNOTTY  0x5422UL
#define TIOCGSID   0x5429UL

/* Linux native x86-64 ioctl encodings; TCGETS transfers 36 bytes. */
#define TCGETS     0x5401UL
#define TCSETS     0x5402UL
#define TCSETSW    0x5403UL
#define TCSETSF    0x5404UL
#define TCFLSH     0x540BUL
#define LINUX_TCGETS2  0x802c542aUL
#define LINUX_TCSETS2  0x402c542bUL
#define LINUX_TCSETSW2 0x402c542cUL
#define LINUX_TCSETSF2 0x402c542dUL
#define TIOCOUTQ   0x5411UL
#define FIONREAD   0x541BUL
#define FIONBIO    0x5421UL
#define FIONCLEX   0x5450UL
#define FIOCLEX    0x5451UL

/* Unix98 PTY helpers. */
#define TIOCPKT    0x5420UL
#define TIOCGPTN   0x80045430UL
#define TIOCSPTLCK 0x40045431UL
#define TIOCGPKT   0x80045438UL
#define TIOCGPTLCK 0x80045439UL

#define TIOCPKT_DATA       0x00u
#define TIOCPKT_FLUSHREAD  0x01u
#define TIOCPKT_FLUSHWRITE 0x02u
#define TIOCPKT_STOP       0x04u
#define TIOCPKT_START      0x08u
#define TIOCPKT_NOSTOP     0x10u
#define TIOCPKT_DOSTOP     0x20u
#define TIOCPKT_IOCTL      0x40u

#define LINUX_TCIFLUSH  0u
#define LINUX_TCOFLUSH  1u
#define LINUX_TCIOFLUSH 2u

#endif

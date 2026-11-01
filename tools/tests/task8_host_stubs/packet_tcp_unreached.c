/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Trap TCP calls in a PACKET-only probe fixture. */
#define UNREACHED(name) __attribute__((noreturn)) void name(void) { __builtin_trap(); }
UNREACHED(net_socket_poll_fd)
UNREACHED(net_socket_error)
UNREACHED(net_socket_recv)
UNREACHED(net_socket_wait_readable)
UNREACHED(net_socket_peek)
UNREACHED(net_socket_available)

/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Trap a TCP readiness query in fixtures modeling only UDP/UNIX. */
__attribute__((weak,noreturn)) int net_socket_available(int handle)
{ (void)handle;__builtin_trap(); }

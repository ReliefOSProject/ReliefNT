/* SPDX-License-Identifier: Apache-2.0 */
/* These unrelated host paths must remain unreachable in this fixture. */
/** @brief Fail if the unmodeled storage_release_task_io path is invoked. */
__attribute__((noreturn)) void storage_release_task_io(void) { __builtin_trap(); }

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SECCOMP_BPF_H
#define RELIEFNT_SECCOMP_BPF_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

/**
 * @brief Check classic BPF opcodes, accesses and all forward paths.
 * @param program Immutable candidate instructions, required.
 * @param length Number of instructions (1 through BPF_MAXINSNS).
 * @param path_masks Caller-owned workspace for length elements, required.
 * @return Zero for an admissible program, otherwise negative EINVAL.
 */
int seccomp_bpf_validate(const struct sock_filter *program, size_t length,
                         uint16_t *path_masks);
/**
 * @brief Run a previously verified program over native syscall metadata.
 * @param program Immutable instructions admitted by seccomp_bpf_validate.
 * @param length Number of instructions in program.
 * @param data Kernel-owned syscall snapshot, required.
 * @return The full BPF return word (action and attached data).
 */
uint32_t seccomp_bpf_run(const struct sock_filter *program, size_t length,
                         const struct seccomp_data *data);

#endif

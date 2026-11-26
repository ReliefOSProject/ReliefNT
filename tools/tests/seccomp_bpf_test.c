/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <linux/seccomp.h>
#include <linux/filter.h>
#include <reliefnt/seccomp_bpf.h>

#define CHECK(condition, label) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, label); \
        return 1; \
    } \
} while (0)

static int validate(const struct sock_filter *program, size_t length)
{
    uint16_t masks[BPF_MAXINSNS];
    return seccomp_bpf_validate(program, length, masks);
}

static int test_return_actions_and_syscall_data(void)
{
    struct sock_filter program[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 39, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct seccomp_data data = {.nr = 39, .arch = 0xc000003e};

    CHECK(validate(program, sizeof(program) / sizeof(program[0])) == 0,
          "valid syscall-number filter rejected");
    CHECK(seccomp_bpf_run(program, sizeof(program) / sizeof(program[0]), &data) ==
              (SECCOMP_RET_ERRNO | EPERM),
          "ERRNO action or syscall-number load changed");
    data.nr = 60;
    CHECK(seccomp_bpf_run(program, sizeof(program) / sizeof(program[0]), &data) ==
              SECCOMP_RET_ALLOW,
          "non-matching syscall was not allowed");
    return 0;
}

static int test_arguments_and_scratch_memory(void)
{
    struct sock_filter program[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, args[0]) + 4),
        BPF_STMT(BPF_ST, 0),
        BPF_STMT(BPF_LD | BPF_MEM, 0),
        BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 1),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x12345679, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
    };
    struct seccomp_data data = {.nr = 1, .args = {UINT64_C(0x1234567812345678)}};

    CHECK(validate(program, sizeof(program) / sizeof(program[0])) == 0,
          "valid argument/scratch filter rejected");
    CHECK(seccomp_bpf_run(program, sizeof(program) / sizeof(program[0]), &data) ==
              SECCOMP_RET_ALLOW,
          "64-bit argument high word or scratch memory was evaluated incorrectly");
    data.args[0] = UINT64_C(0x1234567912345677);
    CHECK(seccomp_bpf_run(program, sizeof(program) / sizeof(program[0]), &data) ==
              (SECCOMP_RET_ERRNO | EACCES),
          "argument mismatch did not take the deny branch");
    return 0;
}

static int test_validation_rejects_malformed_programs(void)
{
    struct sock_filter invalid_jump[] = {
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 9),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_filter invalid_load[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, sizeof(struct seccomp_data)),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_filter divide_by_zero[] = {
        BPF_STMT(BPF_ALU | BPF_DIV | BPF_K, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_filter invalid_opcode[] = {
        BPF_STMT(0xff, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_filter bad_shift[] = {
        BPF_STMT(BPF_ALU | BPF_LSH | BPF_K, 32),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_filter wrong_class[] = {
        BPF_STMT(0x100 | BPF_ALU | BPF_ADD | BPF_K, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_filter initialized_on_one_path[] = {
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),
        BPF_STMT(BPF_ST, 0),
        BPF_STMT(BPF_LD | BPF_MEM, 0),
        BPF_STMT(BPF_RET | BPF_A, 0),
    };
    struct sock_filter unsupported_mod[] = {
        BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, 2),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_filter ignored_jump_bytes[] = {
        BPF_JUMP(BPF_JMP | BPF_JA, 0, 255, 255),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_filter fallthrough[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_IMM, 1),
    };

    CHECK(validate(invalid_jump, 2) == -EINVAL,
          "out-of-range conditional jump accepted");
    CHECK(validate(invalid_load, 2) == -EINVAL,
          "out-of-range seccomp_data load accepted");
    CHECK(validate(divide_by_zero, 2) == -EINVAL,
          "constant division by zero accepted");
    CHECK(validate(invalid_opcode, 2) == -EINVAL,
          "unknown opcode accepted");
    CHECK(validate(fallthrough, 1) == -EINVAL,
          "program without a terminating return accepted");
    CHECK(validate(bad_shift, 2) == -EINVAL,
          "immediate shift outside the 32-bit ALU domain accepted");
    CHECK(validate(wrong_class, 2) == -EINVAL,
          "reserved opcode bits accepted");
    CHECK(validate(initialized_on_one_path, 4) == -EINVAL,
          "scratch load not initialized on all incoming paths accepted");
    CHECK(validate(unsupported_mod, 2) == -EINVAL,
          "socket-only classic BPF MOD accepted by seccomp");
    CHECK(validate(ignored_jump_bytes, 2) == 0,
          "unused JA jt/jf bytes changed validation");
    CHECK(validate(NULL, 1) == -EINVAL,
          "null program accepted");
    CHECK(validate(invalid_load, 0) == -EINVAL,
          "empty program accepted");
    return 0;
}

static int test_division_and_register_shift(void)
{
    const struct seccomp_data data = {0};
    const struct sock_filter div_zero_x[] = {
        BPF_STMT(BPF_LD | BPF_IMM, 8),
        BPF_STMT(BPF_ALU | BPF_DIV | BPF_X, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    const struct sock_filter shift_x[] = {
        BPF_STMT(BPF_LD | BPF_IMM, 3),
        BPF_STMT(BPF_LDX | BPF_IMM, 33),
        BPF_STMT(BPF_ALU | BPF_LSH | BPF_X, 0),
        BPF_STMT(BPF_RET | BPF_A, 0),
    };
    CHECK(seccomp_bpf_run(div_zero_x, 3, &data) == 0,
          "DIV X with a zero divisor did not immediately return zero");
    CHECK(seccomp_bpf_run(shift_x, 4, &data) == 6,
          "register shift did not use native 32-bit shift count semantics");
    return 0;
}

int main(void)
{
    if (test_return_actions_and_syscall_data() ||
        test_arguments_and_scratch_memory() ||
        test_validation_rejects_malformed_programs() ||
        test_division_and_register_shift()) return 1;
    puts("PASS seccomp classic-BPF verification and evaluation");
    return 0;
}

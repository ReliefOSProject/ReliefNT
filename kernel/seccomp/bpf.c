/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/seccomp_bpf.h>
#include <linux/errno.h>

/** @brief Check that a relative jump from this instruction stays in bounds.
 * @param index Current instruction. @param offset Forward displacement.
 * @param length Instruction count. @return True for an in-range target.
 */
static bool bpf_jump_target_valid(size_t index, uint32_t offset, size_t length)
{
    return index + 1 < length && offset < length - index - 1;
}

/** @brief Recognize the exact permitted classic BPF branch encodings.
 * @param code Full 16-bit opcode. @return True for a supported conditional branch.
 */
static bool bpf_cond_jump(uint16_t code)
{
    uint16_t op = BPF_OP(code);
    return code == (BPF_JMP | op | BPF_SRC(code)) &&
           (op == BPF_JEQ || op == BPF_JGT || op == BPF_JGE || op == BPF_JSET);
}

/** @brief Reject unsupported encodings, invalid operands and unsafe control flow.
 * @param program Immutable candidate instructions, required.
 * @param length Number of instructions (1 through BPF_MAXINSNS).
 * @param path_masks Caller-owned workspace for length elements, required.
 * @return Zero on admission, otherwise negative EINVAL.
 */
int seccomp_bpf_validate(const struct sock_filter *program, size_t length,
                         uint16_t *path_masks)
{
    if (!program || !length || length > BPF_MAXINSNS || !path_masks)
        return -LINUX_EINVAL;

    /* Every jump is forward. Intersect the initialized cells of predecessor
     * paths before checking a load, without adding anything to the kernel stack. */
    __builtin_memset(path_masks, 0xff, length * sizeof(*path_masks));
    uint16_t initialized = 0;
    for (size_t i = 0; i < length; ++i) {
        const struct sock_filter *insn = &program[i];
        uint16_t code = insn->code;
        initialized &= path_masks[i];
        switch (BPF_CLASS(code)) {
        case BPF_LD:
            if (code == (BPF_LD | BPF_W | BPF_IMM) ||
                code == (BPF_LD | BPF_W | BPF_LEN)) break;
            if (code == (BPF_LD | BPF_W | BPF_ABS)) {
                if ((insn->k & 3u) || insn->k > sizeof(struct seccomp_data) - sizeof(uint32_t))
                    return -LINUX_EINVAL;
                break;
            }
            if (code == (BPF_LD | BPF_MEM) && insn->k < BPF_MEMWORDS &&
                (initialized & (1u << insn->k))) break;
            return -LINUX_EINVAL;
        case BPF_LDX:
            if (code == (BPF_LDX | BPF_W | BPF_IMM) ||
                code == (BPF_LDX | BPF_W | BPF_LEN) ||
                (code == (BPF_LDX | BPF_MEM) && insn->k < BPF_MEMWORDS &&
                 (initialized & (1u << insn->k))))
                break;
            return -LINUX_EINVAL;
        case BPF_ST:
        case BPF_STX:
            if (code != BPF_CLASS(code) || insn->k >= BPF_MEMWORDS) return -LINUX_EINVAL;
            initialized |= (uint16_t)(1u << insn->k);
            break;
        case BPF_ALU: {
            uint16_t op = BPF_OP(code);
            if (code == (BPF_ALU | BPF_NEG)) break;
            if (code != (BPF_ALU | op | BPF_SRC(code))) return -LINUX_EINVAL;
            if (op != BPF_ADD && op != BPF_SUB && op != BPF_MUL && op != BPF_DIV &&
                op != BPF_OR && op != BPF_AND && op != BPF_LSH && op != BPF_RSH &&
                op != BPF_XOR) return -LINUX_EINVAL;
            if (BPF_SRC(code) == BPF_K &&
                ((op == BPF_DIV && !insn->k) ||
                 ((op == BPF_LSH || op == BPF_RSH) && insn->k >= 32)))
                return -LINUX_EINVAL;
            break;
        }
        case BPF_JMP:
            if (code == (BPF_JMP | BPF_JA)) {
                if (!bpf_jump_target_valid(i, insn->k, length))
                    return -LINUX_EINVAL;
                path_masks[i + 1 + insn->k] &= initialized;
                initialized = UINT16_MAX;
                break;
            }
            if (!bpf_cond_jump(code) ||
                !bpf_jump_target_valid(i, insn->jt, length) ||
                !bpf_jump_target_valid(i, insn->jf, length)) return -LINUX_EINVAL;
            path_masks[i + 1 + insn->jt] &= initialized;
            path_masks[i + 1 + insn->jf] &= initialized;
            initialized = UINT16_MAX;
            break;
        case BPF_RET:
            if (code != (BPF_RET | BPF_K) && code != (BPF_RET | BPF_A))
                return -LINUX_EINVAL;
            break;
        case BPF_MISC:
            if (code != (BPF_MISC | BPF_TAX) && code != (BPF_MISC | BPF_TXA))
                return -LINUX_EINVAL;
            break;
        default:
            return -LINUX_EINVAL;
        }
    }

    uint16_t final_code = program[length - 1].code;
    return final_code == (BPF_RET | BPF_K) || final_code == (BPF_RET | BPF_A)
        ? 0 : -LINUX_EINVAL;
}

/** @brief Read an aligned native-endian word from syscall metadata only.
 * @param data Immutable kernel snapshot. @param offset Verified byte offset.
 * @return Loaded 32-bit word.
 */
static uint32_t bpf_abs_word(const struct seccomp_data *data, uint32_t offset)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t value;
    __builtin_memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

/** @brief Interpret an admitted filter without allocating or accessing user memory.
 * @param program Immutable classic BPF instructions admitted by the verifier.
 * @param length Number of verified instructions.
 * @param data Kernel-owned syscall metadata.
 * @return Action and data from the return instruction, or a terminal action
 * for unexpected input.
 */
uint32_t seccomp_bpf_run(const struct sock_filter *program, size_t length,
                         const struct seccomp_data *data)
{
    uint32_t accumulator = 0, index = 0;
    uint32_t scratch[BPF_MEMWORDS] = {0};
    uint32_t pc = 0;
    if (!program || !length || !data) return SECCOMP_RET_KILL_PROCESS;

    while (pc < length) {
        const struct sock_filter *insn = &program[pc];
        uint16_t code = insn->code;
        uint32_t operand = BPF_SRC(code) == BPF_X ? index : insn->k;
        switch (BPF_CLASS(code)) {
        case BPF_LD:
            if (BPF_MODE(code) == BPF_ABS) accumulator = bpf_abs_word(data, insn->k);
            else if (BPF_MODE(code) == BPF_MEM) accumulator = scratch[insn->k];
            else if (BPF_MODE(code) == BPF_LEN) accumulator = sizeof(*data);
            else accumulator = insn->k;
            break;
        case BPF_LDX:
            if (BPF_MODE(code) == BPF_MEM) index = scratch[insn->k];
            else if (BPF_MODE(code) == BPF_LEN) index = sizeof(*data);
            else index = insn->k;
            break;
        case BPF_ST:
            scratch[insn->k] = accumulator;
            break;
        case BPF_STX:
            scratch[insn->k] = index;
            break;
        case BPF_ALU:
            switch (BPF_OP(code)) {
            case BPF_ADD: accumulator += operand; break;
            case BPF_SUB: accumulator -= operand; break;
            case BPF_MUL: accumulator *= operand; break;
            case BPF_DIV:
                if (!operand) return 0;
                accumulator /= operand;
                break;
            case BPF_OR: accumulator |= operand; break;
            case BPF_AND: accumulator &= operand; break;
            case BPF_LSH: accumulator <<= operand & 31u; break;
            case BPF_RSH: accumulator >>= operand & 31u; break;
            case BPF_NEG: accumulator = 0u - accumulator; break;
            case BPF_XOR: accumulator ^= operand; break;
            default: return SECCOMP_RET_KILL_PROCESS;
            }
            break;
        case BPF_JMP:
            if (code == (BPF_JMP | BPF_JA)) {
                pc += insn->k;
            } else {
                bool take;
                switch (BPF_OP(code)) {
                case BPF_JEQ: take = accumulator == operand; break;
                case BPF_JGT: take = accumulator > operand; break;
                case BPF_JGE: take = accumulator >= operand; break;
                case BPF_JSET: take = (accumulator & operand) != 0; break;
                default: return SECCOMP_RET_KILL_PROCESS;
                }
                pc += take ? insn->jt : insn->jf;
            }
            break;
        case BPF_RET:
            return BPF_RVAL(code) == BPF_A ? accumulator : insn->k;
        case BPF_MISC:
            if (BPF_MISCOP(code) == BPF_TAX) index = accumulator;
            else if (BPF_MISCOP(code) == BPF_TXA) accumulator = index;
            else return SECCOMP_RET_KILL_PROCESS;
            break;
        default:
            return SECCOMP_RET_KILL_PROCESS;
        }
        ++pc;
    }
    return SECCOMP_RET_KILL_PROCESS;
}

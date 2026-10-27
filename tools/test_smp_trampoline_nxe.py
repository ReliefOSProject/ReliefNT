#!/usr/bin/env python3
"""Check that AP startup enables EFER.NXE before paging becomes active."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SmpTrampolineNxeTests(unittest.TestCase):
    def test_nxe_is_enabled_before_paging(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-ap-nxe-") as tmp:
            obj = str(Path(tmp) / "smp_trampoline.o")
            subprocess.run(["cc", "-m32", "-c", "arch/x86_64/cpu/smp_trampoline.S",
                            "-o", obj], cwd=ROOT, check=True)
            disassembly = subprocess.run(
                ["objdump", "-dr", "-M", "intel", "--section=.text.smp_trampoline", obj],
                cwd=ROOT, check=True, capture_output=True, text=True,
            ).stdout

        decoded = []
        for line in disassembly.splitlines():
            match = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]{2}", line)
            if match:
                decoded.append((int(match.group(1), 16), line.split("\t")[-1].strip()))
        instructions = [instruction for _, instruction in decoded]

        def instruction_index(pattern):
            for index, instruction in enumerate(instructions):
                if re.fullmatch(pattern, instruction):
                    return index
            self.fail(f"missing trampoline instruction matching {pattern!r}: {instructions}")

        def jump_target(index):
            match = re.match(r"(?:jb|je|jz|jmp)\s+([0-9a-f]+)", instructions[index])
            self.assertIsNotNone(match, f"expected branch instruction, got {instructions[index]!r}")
            return int(match.group(1), 16)

        max_leaf = instruction_index(r"mov\s+eax,0x80000000")
        check_leaf = instruction_index(r"cmp\s+eax,0x80000001")
        max_leaf_reject = next(i for i in range(check_leaf + 1, len(instructions))
                               if instructions[i].startswith("jb "))
        nx_leaf = instruction_index(r"mov\s+eax,0x80000001")
        nx_capability = instruction_index(r"test\s+edx,0x100000")
        nx_reject = next(i for i in range(nx_capability + 1, len(instructions))
                         if instructions[i].startswith(("je ", "jz ")))
        efer = instruction_index(r"mov\s+ecx,0xc0000080")
        enable_lme_nxe = instruction_index(r"or\s+eax,0x900")
        no_nx_halt = instruction_index(r"hlt")
        no_nx_cli = next(i for i in range(max_leaf_reject + 1, no_nx_halt + 1)
                         if instructions[i] == "cli")
        read_cr0 = instruction_index(r"mov\s+eax,cr0")
        paging_writes = []
        for index, instruction in enumerate(instructions):
            match = re.fullmatch(r"or\s+eax,0x([0-9a-f]+)", instruction)
            if match and int(match.group(1), 16) & (1 << 31):
                paging_writes.append((index, int(match.group(1), 16)))
        self.assertEqual(len(paging_writes), 1, "one AP CR0 paging enable is required")
        enable_paging, cr0_bits = paging_writes[0]
        self.assertTrue(cr0_bits & (1 << 16), "AP supervisor writes must honor read-only PTEs")
        load_cr0 = instruction_index(r"mov\s+cr0,eax")
        efer_writes = [i for i, instruction in enumerate(instructions)
                       if re.fullmatch(r"wrmsr", instruction)]
        nx_path_jump = next(i for i in range(efer_writes[0] + 1, no_nx_halt)
                            if instructions[i].startswith("jmp "))

        self.assertLess(max_leaf, check_leaf)
        self.assertLess(check_leaf, nx_leaf)
        self.assertLess(nx_leaf, nx_capability)
        self.assertLess(nx_capability, nx_reject)
        self.assertLess(nx_reject, efer)
        self.assertLess(nx_capability, efer)
        self.assertLess(efer, enable_lme_nxe)
        self.assertEqual(len(efer_writes), 1)
        self.assertLess(enable_lme_nxe, efer_writes[0])
        self.assertLess(efer_writes[0], nx_path_jump)
        self.assertLess(max_leaf_reject, no_nx_halt)
        self.assertLess(nx_reject, no_nx_halt)
        self.assertEqual(jump_target(max_leaf_reject), decoded[no_nx_cli][0])
        self.assertEqual(jump_target(nx_reject), decoded[no_nx_cli][0])
        self.assertEqual(jump_target(nx_path_jump), decoded[read_cr0][0])
        self.assertLess(read_cr0, enable_paging)
        self.assertLess(no_nx_halt, enable_paging)
        self.assertLess(enable_paging, load_cr0)


if __name__ == "__main__":
    unittest.main()

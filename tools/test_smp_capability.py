#!/usr/bin/env python3
"""Execute the assembled AP capability path with simulated CPUID responses."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_INSN
    from unicorn.x86_const import (UC_X86_INS_CPUID, UC_X86_REG_EAX, UC_X86_REG_EBX,
                                   UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EIP)
except ImportError:
    Uc = None

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipIf(Uc is None, "optional Unicorn emulator is unavailable")
class ApCapability(unittest.TestCase):
    def test_width_and_nx_reject_before_paging(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-ap-cap-") as directory:
            obj = Path(directory) / "ap.o"
            raw = Path(directory) / "ap.bin"
            source = os.environ.get("TRAMPOLINE_IMPL", "arch/x86_64/cpu/smp_trampoline.S")
            subprocess.run(["cc", "-m32", "-c", source, "-o", str(obj)], cwd=ROOT, check=True)
            subprocess.run(["objcopy", "--dump-section", f".text.smp_trampoline={raw}", str(obj)],
                           cwd=ROOT, check=True)
            symbols = {}
            for line in subprocess.check_output(["nm", "-n", str(obj)], text=True).splitlines():
                fields = line.split()
                if len(fields) == 3:
                    symbols[fields[2]] = int(fields[0], 16)
            machine = bytearray(raw.read_bytes())
            if "smp_trampoline_physbits_imm" in symbols:
                machine[symbols["smp_trampoline_physbits_imm"]] = 46
            start = 0x10000 + symbols["smp_trampoline_cr3_imm"] + 7

            def run(width, max_leaf=0x80000008, nx=True):
                cpu = Uc(UC_ARCH_X86, UC_MODE_32)
                cpu.mem_map(0x10000, 0x1000)
                cpu.mem_write(0x10000, bytes(machine))
                outcome = {"parked": False, "paging": False, "efer": 0}

                def cpuid(uc, _):
                    leaf = uc.reg_read(UC_X86_REG_EAX)
                    uc.reg_write(UC_X86_REG_EAX, max_leaf if leaf == 0x80000000 else width)
                    uc.reg_write(UC_X86_REG_EBX, 0)
                    uc.reg_write(UC_X86_REG_ECX, 0)
                    uc.reg_write(UC_X86_REG_EDX, (1 << 20) if leaf == 0x80000001 and nx else 0)
                    return 1

                def instructions(uc, address, size, _):
                    code = bytes(uc.mem_read(address, size))
                    if code == b"\xf4":
                        outcome["parked"] = True
                        uc.emu_stop()
                    elif code == b"\x0f\x32":  # RDMSR
                        uc.reg_write(UC_X86_REG_EAX, 0)
                        uc.reg_write(UC_X86_REG_EDX, 0)
                        uc.reg_write(UC_X86_REG_EIP, address + size)
                    elif code == b"\x0f\x30":  # WRMSR
                        outcome["efer"] = uc.reg_read(UC_X86_REG_EAX)
                        uc.reg_write(UC_X86_REG_EIP, address + size)
                    elif code == b"\x0f\x22\xc0":  # MOV CR0, EAX
                        outcome["paging"] = bool(uc.reg_read(UC_X86_REG_EAX) & (1 << 31))
                        self.assertTrue(uc.reg_read(UC_X86_REG_EAX) & (1 << 16))
                        uc.emu_stop()

                cpu.hook_add(UC_HOOK_INSN, cpuid, None, 1, 0, UC_X86_INS_CPUID)
                cpu.hook_add(UC_HOOK_CODE, instructions)
                cpu.emu_start(start, 0x11000, count=100)
                return outcome

            for width in (32, 45, 53, 255):
                result = run(width)
                self.assertTrue(result["parked"] and not result["paging"] and not result["efer"])
            for result in (run(46, max_leaf=0x80000001), run(46, nx=False)):
                self.assertTrue(result["parked"] and not result["paging"] and not result["efer"])
            for width in (46, 52):
                result = run(width)
                self.assertTrue(result["paging"] and not result["parked"])
                self.assertEqual(result["efer"] & 0x900, 0x900)


if __name__ == "__main__":
    unittest.main()

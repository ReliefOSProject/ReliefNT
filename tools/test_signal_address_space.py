#!/usr/bin/env python3
"""Host regressions for the real ReliefNT implementation."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SignalAddressSpaceTests(unittest.TestCase):
    def test_signal_frame_uses_destination_address_space(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-signal-") as tmp:
            executable = str(Path(tmp) / "signal")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
                "tools/tests/signal_address_space_test.c", "kernel/syscall/ipc/sysv_sem.c",
                "kernel/syscall/fs/locks.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)



if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Host regressions for the real ReliefNT implementation."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GpuAbiTests(unittest.TestCase):
    def test_syscall_and_copyout(self):
        for name, source in (("gpu_syscall", "drivers/video/core/gpu.c"),
                             ("gpu_usercopy", "kernel/exec/usercopy.c")):
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix="reliefos-gpu-") as tmp:
                executable = str(Path(tmp) / name)
                subprocess.run([
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-Iinclude", "-Iinclude", "-Iinclude", "-Iinclude/uapi",
                    f"tools/tests/{name}_test.c", source, "-o", executable,
                ], cwd=ROOT, check=True)
                subprocess.run([executable], cwd=ROOT, check=True, timeout=30)



if __name__ == "__main__":
    unittest.main()

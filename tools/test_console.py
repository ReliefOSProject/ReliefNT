#!/usr/bin/env python3
"""Host regressions for the real ReliefNT implementation."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ConsoleTests(unittest.TestCase):
    def test_console_cursor_and_background_output(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-vt-console-") as tmp:
            binary = str(Path(tmp) / "console")
            subprocess.run(["clang", "-std=c11", "-g", "-O1",
                            "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
                            "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
                            "-Wl,--gc-sections", "tools/tests/vt_console_test.c", "-o", binary],
                           cwd=ROOT, check=True)
            subprocess.run([binary], check=True, timeout=10)



if __name__ == "__main__":
    unittest.main()

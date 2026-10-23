#!/usr/bin/env python3
"""Host regressions for the real ReliefNT implementation."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InputTests(unittest.TestCase):
    def test_keyboard_led_ioctl(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-keyboard-led-") as tmp:
            executable = str(Path(tmp) / "led")
            subprocess.run([
                "clang", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
                "tools/tests/evdev_led_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)


    def test_keyboard_irq_rejects_empty_and_auxiliary_output(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-keyboard-irq-") as tmp:
            executable = str(Path(tmp) / "keyboard-irq")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Iinclude", "-Iinclude", "-Iinclude/uapi",
                "-Iinclude",
                "tools/tests/keyboard_irq_ready_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)


    def test_mouse_irq_leaves_interleaved_keyboard_bytes_for_irq1(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-ps2-interleave-") as tmp:
            executable = str(Path(tmp) / "ps2-interleave")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Iinclude", "-Iinclude", "-Iinclude/uapi",
                "-Iinclude",
                "tools/tests/ps2_interleaved_irq_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)


    def test_mouse_parameters_reach_auxiliary_port(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-mouse-") as tmp:
            executable = str(Path(tmp) / "mouse-init")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
                "tools/tests/mouse_init_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)



if __name__ == "__main__":
    unittest.main()

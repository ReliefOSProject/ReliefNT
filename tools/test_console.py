#!/usr/bin/env python3
"""Host regressions for the real ReliefNT implementation."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ConsoleTests(unittest.TestCase):
    def test_framebuffer_character_edits(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-fb-edit-") as tmp:
            generated = Path(tmp) / "generated"
            generated.mkdir()
            (generated / "cjk_font.h").write_text(
                "static const struct { uint32_t cp; uint8_t width; uint8_t bits[32]; } "
                "cjk_glyphs[] = {{0x4e2d, 2, {0xff}}, {0xfffd, 1, {0}}};\n"
            )
            binary = str(Path(tmp) / "edits")
            subprocess.run(["clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra",
                            "-Werror", "-ffunction-sections", "-fdata-sections",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-Iinclude", "-Iinclude/uapi", f"-I{tmp}",
                            "-Idrivers/video/framebuffer", "-Wl,--gc-sections",
                            "tools/tests/framebuffer_edit_test.c", "-o", binary],
                           cwd=ROOT, check=True)
            for bpp in (24, 32):
                subprocess.run([binary, str(bpp)], check=True, timeout=10)
            command = ["clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                       "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
                       "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi", f"-I{tmp}",
                       "-Idrivers/video/framebuffer", "-Wl,--gc-sections",
                       "tools/tests/console_equivalence_test.c", "-o", binary]
            subprocess.run(command, cwd=ROOT, check=True)
            subprocess.run([binary], check=True, timeout=30)

    def test_serial_transmit_queue(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-serial-") as tmp:
            binary = str(Path(tmp) / "serial")
            subprocess.run([
                "clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-Iinclude", "-Iinclude/uapi", "-no-pie",
                "tools/tests/serial_tx_test.c", "kernel/device/serial_tx.c", "-o", binary,
            ], cwd=ROOT, check=True)
            subprocess.run([binary], check=True, timeout=10)

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

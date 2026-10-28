"""Exercise production VGA text state and hardware without accessing host ports."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class VgaTextTests(unittest.TestCase):
    def test_hardware(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-vga-hw-") as tmp:
            binary=Path(tmp)/"hardware"
            subprocess.run(["clang","-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
                "-fsanitize=address,undefined","-fno-sanitize-recover=all","-Iinclude","-Iinclude/uapi",
                "tools/tests/vga_hardware_test.c","drivers/video/vga/text_mode.c","-o",str(binary)],cwd=ROOT,check=True)
            for case in ("vbe","svga","unsupported","svga-busy"):
                subprocess.run([binary,case],check=True,timeout=20)

    def test_console_integration(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-vga-vt-") as tmp:
            binary=Path(tmp)/"console"
            subprocess.run(["clang","-std=c11","-O1","-g","-fsanitize=address,undefined",
                "-fno-sanitize-recover=all","-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
                "-Iinclude","-Iinclude/uapi","tools/tests/vga_console_test.c","-o",str(binary)],cwd=ROOT,check=True)
            subprocess.run([binary],check=True,timeout=20)

    def test_terminal(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-vga-") as tmp:
            binary = Path(tmp) / "terminal"
            subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/vga_terminal_test.c", "drivers/console/vga_terminal.c",
                "drivers/console/vga_ansi.c", "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([binary], check=True, timeout=20)

if __name__ == "__main__":
    unittest.main()

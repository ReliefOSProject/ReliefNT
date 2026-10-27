#!/usr/bin/env python3
"""Check reproducible native module ABI/release identity and invalidation."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/module-abi.sh"


class ModuleIdentity(unittest.TestCase):
    def test_reproducible_and_changes_invalidate(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-module-identity-") as directory:
            base = Path(directory)
            src = base / "source"
            for name in ("arch", "block", "drivers", "firmware", "fs", "kernel", "lib", "mm", "net", "sound", "vdso", "include", "configs", "scripts"):
                (src / name).mkdir(parents=True, exist_ok=True)
            (src / "kernel/core.c").write_text("int native_interface(int x) { return x; }\n")
            (src / "include/api.h").write_text("int native_interface(int x);\n")
            (src / "Kbuild").write_text("obj-y := kernel/\n")
            (src / "Kconfig").write_text('mainmenu "ReliefNT"\n')
            (src / "Makefile").write_text("# native build adapter\n")
            (src / "scripts/Makefile.reliefnt").write_text("# native compile flags\n")
            (src / "scripts/Makefile.port").write_text("# upstream adapter\n")
            (src / "scripts/kbuild-source.json").write_text("{}\n")
            (src / "configs/build-version").write_text("release_version=5.1.2\n")
            out = base / "output"
            out.mkdir()
            (out / ".config").write_text("# generated\nCONFIG_MODULES=y\n")
            for tool in ("cc", "ar", "ld", "objcopy"):
                (out / f".reliefnt-target-{tool}.id").write_text("a" * 64 + "\n")
            def run(source=src, flags="release", extra=""):
                result = subprocess.run(["sh", str(SCRIPT), str(source), str(out), flags, extra, ""],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return (out / "include/generated/module_abi.h").read_bytes()
            before = run()
            self.assertIn(b'RELIEFNT_KERNEL_RELEASE "5.1.2-ko1-', before)
            stamp = (out / "include/generated/module_abi.h").stat().st_mtime_ns
            self.assertEqual(before, run())
            self.assertEqual(stamp, (out / "include/generated/module_abi.h").stat().st_mtime_ns)
            archive = base / "relocated source"
            shutil.copytree(src, archive)
            self.assertEqual(before, run(archive))
            for tool in ("cc", "ar", "ld", "objcopy"):
                identity_file = out / f".reliefnt-target-{tool}.id"
                identity_file.write_text("b" * 64 + "\n")
                self.assertNotEqual(before, run(), tool + " must invalidate module identity")
                identity_file.write_text("a" * 64 + "\n")
                self.assertEqual(before, run())
            self.assertNotEqual(before, run(flags="debug"))
            (src / "include/api.h").write_text("long native_interface(long x);\n")
            changed = run()
            self.assertNotEqual(before, changed)
            (out / ".config").write_text("CONFIG_MODULES=y\nCONFIG_DRIVER_TEST=m\n")
            self.assertNotEqual(changed, run())
            result = subprocess.run(["sh", str(SCRIPT), str(src), str(out), "release", "-abcdefghijklmnopqrstuvwxyz", ""],
                                    capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            release = (out / "include/config/kernel.release").read_text().strip()
            self.assertLessEqual(len(release), 31)


if __name__ == "__main__":
    unittest.main()

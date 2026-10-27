#!/usr/bin/env python3
"""Check installed product identity independently of the kernel compilation engine."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InstallBundle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="reliefnt-native-install-bundle-")
        cls.addClassCleanup(cls.work.cleanup)
        cls.binary = Path(cls.work.name) / "install-bundle"
        subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-Wpedantic", "-Wformat=2", "-Wshadow", "-Wstrict-prototypes", "-Wmissing-prototypes",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        str(ROOT / "tools/host/manifest/install-bundle.c"), "-o", str(cls.binary)], check=True)

    def test_managed_files_and_corruption(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-install-bundle-") as directory:
            dest = Path(directory)
            release = "5.1.2-ko1-0123456789ab"
            modules = dest / "lib/modules" / release
            (modules / "kernel/fs").mkdir(parents=True)
            for name, content in {"config": b"CONFIG_MODULES=y\nCONFIG_EXT4_FS=m\n",
                                  "module.abi": b"0123456789ab" + b"0" * 52 + b"\n",
                                  "modules.order": b"kernel/fs/ext4.ko\n",
                                  "modules.builtin": b"kernel/builtin.ko\n",
                                  "modules.builtin.modinfo": b"builtin.name=builtin\0",
                                  "Module.symvers": b"", "System.map": b"",
                                  "kernel/fs/ext4.ko": b"test metadata carrier"}.items():
                (modules / name).write_bytes(content)
            ownership = modules / ".reliefnt-kernel.manifest"
            ownership.write_text("".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " +
                                          str(p.relative_to(modules)) + "\n"
                                          for p in sorted(modules.rglob("*")) if p.is_file()))
            for name in ("kernel.sys", "kernel.debug", "kerneldebug.sys"):
                (dest / name).write_bytes(name.encode())

            def run():
                return subprocess.run([str(self.binary), str(dest),
                                       release, "x86_64", "debug"], text=True, capture_output=True)

            result = run()
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            manifest = json.loads((dest / "install.bundle.json").read_text())
            self.assertEqual(manifest["release"], release)
            self.assertEqual(manifest["modules"], [f"lib/modules/{release}/kernel/fs/ext4.ko"])
            self.assertEqual(manifest["configuration"]["EXT4_FS"], "m")
            self.assertEqual(manifest["files"]["kernel.sys"]["sha256"],
                             hashlib.sha256(b"kernel.sys").hexdigest())
            before = (dest / "install.bundle.json").read_bytes()
            (modules / "kernel/fs/ext4.ko").write_bytes(b"corrupt")
            self.assertNotEqual(run().returncode, 0)
            self.assertEqual((dest / "install.bundle.json").read_bytes(), before)
            (modules / "config").unlink()
            (modules / "config").symlink_to(dest / "kernel.sys")
            self.assertNotEqual(run().returncode, 0)


if __name__ == "__main__":
    unittest.main()

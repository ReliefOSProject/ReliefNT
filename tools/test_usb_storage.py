#!/usr/bin/env python3
"""Exercise separately compiled BOT/SCSI code through a scripted USB host."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class USBStorage(unittest.TestCase):
    def test_scripted_bot_scsi(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-usb-storage-") as directory:
            sources = ["tools/tests/usb_storage_test.c"]
            # During RED the class may not yet exist; unresolved weak entry points
            # produce the intentional feature-missing assertion in the fixture.
            sources += [str(path.relative_to(ROOT)) for path in (
                ROOT / "drivers/usb/storage/bulk_only.c",
                ROOT / "drivers/usb/storage/scsi.c",
            ) if path.exists()]
            for real_registry in (False, True):
                with self.subTest(real_registry=real_registry):
                    binary = Path(directory) / ("registry" if real_registry else "scripted")
                    extra = (["-DREAL_REGISTRY", "-DCONFIG_MODULES=1",
                              "block/device/registry.c"] if real_registry else [])
                    command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                               "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                               "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                               "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                               "-Iinclude", "-Iinclude/uapi", *sources, *extra, "-o", str(binary)]
                    build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         timeout=30)
                    self.assertEqual(run.returncode, 0, run.stdout)
                    self.assertIn("USB storage scripted tests passed", run.stdout)


if __name__ == "__main__":
    unittest.main()

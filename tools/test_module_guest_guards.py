#!/usr/bin/env python3
"""Prove guest-only module/media probes refuse Linux before mutating syscalls."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class GuestGuards(unittest.TestCase):
    def test_no_host_module_or_filesystem_mutations(self):
        tracer = shutil.which("strace")
        if not tracer:
            self.skipTest("strace is needed to observe refusal before all mutators")
        with tempfile.TemporaryDirectory(prefix="reliefnt-guest-guards-") as directory:
            work = Path(directory)
            for name, source, flags in (
                ("initramfs", "initramfs_runtime_probe.c", []),
                ("boot", "module_boot_runtime_probe.c", []),
                ("boot_parameters", "module_boot_runtime_probe.c", ["-DRELIEFNT_PARAMETER_RUNTIME_TEST=1"]),
                ("boot_sound_disabled", "module_boot_runtime_probe.c", ["-DRELIEFNT_SOUND_DISABLED_RUNTIME_TEST=1"]),
                ("boot_drm_disabled", "module_boot_runtime_probe.c", ["-DRELIEFNT_DRM_DISABLED_RUNTIME_TEST=1"]),
                ("boot_no_modules", "module_boot_runtime_probe.c", ["-DRELIEFNT_NO_MODULES_RUNTIME_TEST=1"]),
                ("boot_allmod", "module_boot_runtime_probe.c", ["-DRELIEFNT_ALLMOD_RUNTIME_TEST=1"]),
                ("drm", "module_drm_probe.c", ["-Iinclude/uapi"]),
                ("hda", "module_hda_probe.c", ["-Iinclude/uapi"]),
                ("drm_contract", "drm_runtime_probe.c", ["-Iinclude/uapi"]),
                ("filesystems", "module_filesystem_owner_probe.c", []),
                ("nvme", "module_block_owner_probe.c", []),
                ("ide", "module_block_owner_probe.c", ["-DMODULE_BLOCK_TRANSPORT_IDE=1"]),
                ("ahci", "module_block_owner_probe.c", ["-DMODULE_BLOCK_TRANSPORT_AHCI=1"]),
            ):
                with self.subTest(probe=name):
                    binary, trace = work / name, work / (name + ".trace")
                    result = subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-D_GNU_SOURCE=1", "-include", str(ROOT / "include/uapi/linux/stat.h"), "-include", "sys/statvfs.h", *flags, str(ROOT / "tools/tests" / source), "-o", str(binary)], text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    self.assertEqual(result.returncode, 0, result.stdout)
                    result = subprocess.run([tracer, "-qq", "-f", "-e",
                        "trace=init_module,finit_module,delete_module,mount,umount2,unlink,unlinkat,rename,renameat,renameat2,mkdir,mkdirat,mknod,mknodat",
                        "-o", str(trace), str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
                    self.assertEqual(result.returncode, 77, result.stdout)
                    self.assertIn("refusing host mutations", result.stdout)
                    self.assertFalse(re.search(r"\b(?:init_module|finit_module|delete_module|mount|umount2|unlink\w*|rename\w*|mkdir\w*|mknod\w*)\(",
                        trace.read_text()), trace.read_text())


if __name__ == "__main__":
    unittest.main()

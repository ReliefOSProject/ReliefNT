#!/usr/bin/env python3
"""Host regressions for the real ReliefNT implementation."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RootfsInterfaces(unittest.TestCase):
    def test_kernel_modules(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-rootfs-") as directory:
            for name in ("rootfs_mounts", "rootfs_uts", "rootfs_paths", "procfs_directories", "linux_permissions", "chroot_paths"):
                with self.subTest(module=name):
                    executable = Path(directory) / name
                    # This shared fixture now exercises actual retained proc
                    # mounts as well as its original directory contract.
                    extra = (["fs/vfs/mount_tree.c", "fs/vfs/mount_table.c", "fs/vfs/path.c", "fs/vfs/file.c", "fs/vfs/open.c",
                              "fs/vfs/pseudo.c", "fs/vfs/attributes.c", "fs/proc/mount.c", "kernel/sched/identity.c",
                              "fs/security/access.c"] if name in ("procfs_directories", "rootfs_paths") else [])
                    if name in ("linux_permissions", "chroot_paths"):
                        extra = ['fs/vfs/mount_tree.c', 'fs/vfs/mount_table.c', 'fs/vfs/path.c', 'fs/vfs/dentry.c', 'fs/vfs/file.c', 'fs/vfs/open.c', 'fs/vfs/attributes.c', 'fs/vfs/mutation.c', 'fs/security/access.c', 'kernel/syscall/fs/create.c', 'kernel/syscall/fs/mutation.c']
                    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                                    "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude",
                                    f"tools/tests/{name}_test.c", *extra, "-o", executable], cwd=ROOT, check=True)
                    subprocess.run([executable], cwd=ROOT, check=True, timeout=20)



if __name__ == "__main__":
    unittest.main()

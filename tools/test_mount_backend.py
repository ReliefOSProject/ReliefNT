#!/usr/bin/env python3
"""Walk actual retained tmpfs inode objects through the native mount graph."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class MountBackend(unittest.TestCase):
    def test_real_tmpfs_without_text_routing(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-mount-backend-") as directory:
            binary = Path(directory) / "mount-backend"
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", "-Wl,--wrap=storage_ext4_read_inode", "-Wl,--wrap=storage_read_device", "-Wl,--wrap=storage_inode_stat", "-Wl,--wrap=storage_backend_commit_rename_metadata", "-Wl,--wrap=storage_cluster_write", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-Iinclude", "-Iinclude/uapi", "tools/tests/mount_backend_test.c", "kernel/dma/buffer.c", "drivers/storage/ahci/dma.c",
                "fs/vfs/filesystems.c", "fs/vfs/filesystem_instance.c", "fs/ext_common/journal.c", "fs/vfs/mount_tree.c", "fs/vfs/mount_table.c", "fs/vfs/path.c", "fs/vfs/dentry.c", "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/kernel_file.c", "fs/vfs/kernel_path.c", "fs/vfs/exec_file.c", "fs/vfs/attributes.c", "fs/vfs/initial.c", "fs/vfs/storage_initial.c", "fs/vfs/storage_attributes.c", "fs/vfs/storage_access.c", "fs/vfs/storage_backend.c", "fs/vfs/mutation.c", "fs/vfs/storage_mutation.c", "fs/vfs/storage_create.c", "fs/vfs/storage_security.c", "fs/vfs/write_access.c",
                "fs/vfs/storage_mount.c", "fs/tmpfs/core.c", "fs/security/access.c", "lib/text_utf16.c", "fs/devfs/mount.c", "fs/devfs/device.c", "fs/devfs/identity.c", "fs/vfs/pseudo.c", "kernel/exec/runtime.c", "kernel/exec/pending.c", "kernel/syscall/process/exec.c", "kernel/syscall/system/user_string.c",
                "kernel/syscall/ipc/unix_path.c", "kernel/syscall/ipc/unix_name.c", "kernel/syscall/fs/create.c", "kernel/syscall/fs/mount.c", "kernel/syscall/fs/mutation.c", "kernel/syscall/fs/directory.c", "kernel/syscall/fs/size.c", "kernel/syscall/fs/sync.c", "kernel/syscall/fs/path.c", "kernel/syscall/fs/lookup_state.c", "kernel/syscall/fd/descriptor.c", "kernel/device/character.c", "kernel/syscall/io/vector.c", "kernel/syscall/fd/pty_descriptor.c", "kernel/sched/tables.c", "kernel/device/mapping.c", "kernel/sched/fs.c", "kernel/sync/wait.c", "-o", str(binary)
            ], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
            for filesystem in ("ext2", "ext4"):
                image = Path(directory) / (filesystem + ".img")
                result = Path(directory) / (filesystem + ".result")
                with image.open("wb") as output:
                    output.truncate(64 * 1024 * 1024)
                features = "none,filetype,large_file,dir_index"
                if filesystem == "ext4":
                    features += ",extents,64bit,metadata_csum,extra_isize,flex_bg,has_journal"
                subprocess.run(["mke2fs", "-q", "-F", "-t", filesystem, "-b", "4096", "-I", "256",
                                "-O", features, str(image)], check=True)
                subprocess.run([str(binary), str(image), str(result)], check=True, timeout=30)
                subprocess.run(["e2fsck", "-f", "-n", str(result)], check=True)
            source = Path(directory) / "iso-source"
            source.mkdir()
            (source / "demo.ko").write_bytes(b"native module snapshot data!")
            iso = Path(directory) / "readonly.iso"
            subprocess.run(["xorriso", "-as", "mkisofs", "-quiet", "-o", str(iso), str(source)], check=True)
            subprocess.run([str(binary), str(iso)], check=True, timeout=30)
            for filesystem in ("vfat", "exfat"):
                image = Path(directory) / (filesystem + ".img")
                result = Path(directory) / (filesystem + ".result")
                with image.open("wb") as output:
                    output.truncate(64 * 1024 * 1024)
                formatter = ["mkfs.fat", "-F", "32"] if filesystem == "vfat" else ["mkfs.exfat"]
                subprocess.run([*formatter, str(image)], check=True)
                subprocess.run([str(binary), str(image), str(result), filesystem], check=True, timeout=30)
                checker = "fsck.fat" if filesystem == "vfat" else "fsck.exfat"
                subprocess.run([checker, "-n", str(result)], check=True)
if __name__ == "__main__":
    unittest.main()

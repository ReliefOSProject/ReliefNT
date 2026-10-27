#!/usr/bin/env python3
"""Exercise versioned module installation ownership using private directories."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/modules-publish.sh"


class ModulesPublish(unittest.TestCase):
    def test_managed_cleanup_and_user_files(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-modules-publish-") as directory:
            work = Path(directory)
            stage, prefix = work / "stage", work / "root"
            release = "5.1.2-ko1-0123456789ab"
            source = stage / "lib/modules" / release
            dest = prefix / "lib/modules" / release
            (source / "kernel").mkdir(parents=True)
            (source / "kernel/a.ko").write_bytes(b"first")
            (source / "kernel/b.ko").write_bytes(b"removed later")
            (source / "modules.order").write_text("kernel/a.ko\nkernel/b.ko\n")
            def publish(success=True):
                result = subprocess.run(["sh", str(SCRIPT), str(stage), str(prefix), release, "kernel"],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
            publish()
            (dest / "kernel/user.ko").write_bytes(b"user module")
            (dest / "kernel/b.ko").write_bytes(b"user edited")
            (source / "kernel/b.ko").unlink()
            (source / "kernel/a.ko").write_bytes(b"updated")
            publish()
            self.assertEqual((dest / "kernel/a.ko").read_bytes(), b"updated")
            self.assertEqual((dest / "kernel/b.ko").read_bytes(), b"user edited")
            self.assertEqual((dest / "kernel/user.ko").read_bytes(), b"user module")
            (source / "kernel/a.ko").unlink()
            publish()
            self.assertFalse((dest / "kernel/a.ko").exists())
            (source / "kernel/user.ko").write_bytes(b"must not overwrite")
            publish(False)
            self.assertEqual((dest / "kernel/user.ko").read_bytes(), b"user module")

    def test_path_and_symlink_escape_rejected(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-modules-escape-") as directory:
            work = Path(directory)
            stage, prefix = work / "stage", work / "root"
            release = "5.1.2-ko1-0123456789ab"
            source = stage / "lib/modules" / release / "kernel"
            source.mkdir(parents=True)
            (source / "a.ko").write_bytes(b"native module")
            outside = work / "outside"
            outside.mkdir()
            prefix.mkdir()
            (prefix / "lib").symlink_to(outside, target_is_directory=True)
            result = subprocess.run(["sh", str(SCRIPT), str(stage), str(prefix), release, "kernel"], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(list(outside.iterdir()), [])
            result = subprocess.run(["sh", str(SCRIPT), str(stage), str(prefix), "../bad", "kernel"], capture_output=True)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()

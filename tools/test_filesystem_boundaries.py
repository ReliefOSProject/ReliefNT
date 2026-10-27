#!/usr/bin/env python3
"""Link the real permanent core without any optional on-disk filesystem code."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def symbols(path, defined):
    output = subprocess.check_output([
        "nm", "-g", "--defined-only" if defined else "--undefined-only", str(path),
    ], text=True)
    return {line.split()[-1] for line in output.splitlines() if line.split()}


class FilesystemBoundaries(unittest.TestCase):
    def test_permanent_core_does_not_import_optional_code(self):
        core_directories = ("fs/vfs", "fs/security", "block/core", "block/device", "block/partition", "block/install")
        provider_directories = ("fs/fat", "fs/exfat", "fs/ext2", "fs/ext4", "fs/ext_common", "fs/iso9660")
        with tempfile.TemporaryDirectory(prefix="reliefnt-fs-boundaries-") as directory:
            work = Path(directory)
            groups = []
            for directories in (core_directories, provider_directories):
                objects = []
                for folder in directories:
                    for source in sorted((ROOT / folder).glob("*.c")):
                        relative = source.relative_to(ROOT)
                        obj = work / (str(relative).replace("/", "_") + ".o")
                        result = subprocess.run([
                            "clang", "-std=gnu11", "-O1", "-ffreestanding", "-fno-pie",
                            "-fno-stack-protector", "-mno-red-zone", "-mcmodel=large",
                            "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1",
                            "-Iinclude", "-Iinclude/uapi", "-c", str(relative), "-o", str(obj),
                        ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                        self.assertEqual(result.returncode, 0, str(relative) + "\n" + result.stderr)
                        objects.append(obj)
                groups.append(objects)
            linked = work / "permanent-core.o"
            subprocess.run(["ld", "-r", "-o", str(linked), *map(str, groups[0])], check=True)
            providers = set().union(*(symbols(obj, True) for obj in groups[1]))
            imports = symbols(linked, False) & providers
            self.assertEqual(sorted(imports), [], "Optional filesystem definitions imported by permanent core")


if __name__ == "__main__":
    unittest.main()

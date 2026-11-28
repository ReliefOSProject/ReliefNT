#!/usr/bin/env python3
"""Exercise cgroup admission and retirement through the real scheduler."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="reliefnt-cgroup-sched-") as directory:
        binary = Path(directory) / "cgroup-scheduler"
        subprocess.run([
            "clang", "-std=c11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-Iinclude", "-Iinclude/uapi", "tools/tests/cgroup_scheduler_test.c",
            "fs/cgroup/hierarchy.c", "fs/cgroup/filesystem.c", "fs/vfs/lookup_cache.c",
            "fs/vfs/file.c", "-o", str(binary),
        ], cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=20)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Verify production PAT policy with explicit privileged-CPU boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="pat-", dir=os.environ["TMPDIR"]) as directory:
        binary = Path(directory) / "pat"
        subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Iinclude",
                        "tools/tests/pat_test.c", "arch/x86_64/mm/pat.c", "-o", str(binary)],
                       cwd=ROOT, check=True)
        for case in ("normal", "nopat", "unsupported", "layout", "write-failure",
                     "ap-unsupported", "ap-write-failure"):
            subprocess.run([str(binary), case], cwd=ROOT, check=True, timeout=10)
        mapping = Path(directory) / "wc-mapping"
        subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Iinclude", "-Iinclude/uapi",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "tools/tests/framebuffer_wc_mapping_test.c", "-o", str(mapping)],
                       cwd=ROOT, check=True)
        for case in ("normal", "lease", "alias", "ram", "unsupported", "users", "malformed"):
            subprocess.run([str(mapping), case], cwd=ROOT, check=True, timeout=10)


if __name__ == "__main__":
    main()

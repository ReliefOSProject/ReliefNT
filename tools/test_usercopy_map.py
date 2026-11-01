#!/usr/bin/env python3
"""Exercise production task copies with mapped and unmapped physical pages."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-usercopy-map-") as work:
    binary = str(Path(work) / "usercopy")
    override = os.environ.get("USERCOPY_IMPL")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-fno-pie", "-no-pie", "-Iinclude", "-Iinclude/uapi",
                    *([f'-DUSERCOPY_IMPL="{override}"'] if override else []),
                    "tools/tests/usercopy_map_test.c", "-o", binary], cwd=root, check=True)
    subprocess.run([binary], cwd=root, check=True, timeout=20)

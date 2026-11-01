#!/usr/bin/env python3
"""Check real VBE probing, bootstrap reservation and sparse-map staging."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-vbe-boot-") as work:
    binary = str(Path(work) / "test")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/vbe_boot_surface_test.c", "mm/physical/topology.c",
                    "mm/physical/bootstrap.c", "arch/x86_64/mm/direct_map.c",
                    "drivers/video/framebuffer/color.c",
                    "-o", binary], cwd=root, check=True)
    subprocess.run([binary], cwd=root, check=True, timeout=20)
    subprocess.run([binary, "high"], cwd=root, check=True, timeout=20)

#!/usr/bin/env python3
"""Exercise real DRM BO placement through real SVGA borrowed attachment."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-vmwgfx-ppn-") as work:
    binary = str(Path(work) / "test")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/vmwgfx_ppn_bo_test.c", "drivers/video/drm/drm_bo.c",
                    "drivers/video/svga/borrowed.c", "drivers/video/svga/gb.c", "-o", binary],
                   cwd=root, check=True)
    subprocess.run([binary], cwd=root, check=True, timeout=20)

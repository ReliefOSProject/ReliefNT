#!/usr/bin/env python3
"""Exercise ext4 DMA destination, memory failure and fallback boundaries."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="ext4-dma-") as temporary:
    binary = Path(temporary) / "dma"
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-DRELIEFOS_STORAGE_STANDALONE_TU", "-Iinclude", "-Iinclude/uapi",
        "tools/tests/ext4_dma_read_test.c", "-o", str(binary)], cwd=ROOT, check=True)
    for args in ([], ["uncached"], ["no-memory"], ["high-memory"]):
        subprocess.run([str(binary), *args], check=True, timeout=30)

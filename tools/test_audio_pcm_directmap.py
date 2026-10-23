#!/usr/bin/env python3
"""Run a focused PCM DMA/mmap direct-map-address regression."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefos-audio-pcm-directmap-") as directory:
    binary = Path(directory) / "pcm-directmap"
    subprocess.run([
        "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Iinclude", "-Iinclude/uapi", "-Iinclude",
        "tools/tests/audio_pcm_directmap_test.c", "-o", str(binary),
    ], cwd=ROOT, check=True, timeout=30)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=15)

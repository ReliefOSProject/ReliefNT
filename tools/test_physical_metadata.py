#!/usr/bin/env python3
"""Run sparse physical-memory behavior against production PMM code."""
from pathlib import Path
import subprocess
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-pmm-") as work:
    binary = Path(work) / "physical_metadata_test"
    compiler = shutil.which("clang") or "cc"
    common = [compiler, "-std=c11", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
              "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
              "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
              "-Iinclude", "-Iinclude/uapi"]
    bits_binary = Path(work) / "physical_bits_test"
    subprocess.run([*common, "-Wl,--gc-sections", "tools/tests/physical_bits_test.c",
                    "arch/x86_64/mm/paging.c", "-o", str(bits_binary)], cwd=ROOT, check=True)
    subprocess.run([str(bits_binary)], cwd=ROOT, check=True, timeout=30)
    heap_object = Path(work) / "heap.o"
    subprocess.run([*common, "-Dmm_virt_to_phys=fixture_heap_virt_to_phys", "-c",
                    "mm/heap/heap.c", "-o", str(heap_object)], cwd=ROOT, check=True)
    subprocess.run([*common, "-Wl,--gc-sections",
                    "tools/tests/physical_metadata_test.c", "mm/physical/mm.c",
                    "mm/physical/metadata.c", "mm/physical/topology.c",
                    "mm/physical/bootstrap.c", "arch/x86_64/boot/multiboot2.c", str(heap_object),
                    "mm/heap/slab.c", "mm/heap/tree.c",
                    "-o", str(binary)], cwd=ROOT, check=True)
    failed = False
    for case in ("translation", "framebuffer", "small", "high", "ownership", "saturation",
                 "crossings", "crossing_partial", "fragments", "concurrency", "model", "tail", "failure",
                 "init_panic", "heap", "wide", "clipped", "mapped_sparse_runs"):
        print(f"CASE {case}", flush=True)
        failed |= subprocess.run([str(binary), case], cwd=ROOT, timeout=30).returncode != 0
    if failed:
        raise SystemExit(1)

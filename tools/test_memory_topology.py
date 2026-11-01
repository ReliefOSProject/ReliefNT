#!/usr/bin/env python3
"""Exercise the production topology and bootstrap implementation on host RAM."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="reliefnt-topology-") as tmp:
    binary = Path(tmp) / "memory_topology_test"
    subprocess.run([
        "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/memory_topology_test.c",
        "mm/physical/topology.c", "mm/physical/bootstrap.c",
        "arch/x86_64/boot/multiboot2.c", "-o", str(binary),
    ], cwd=ROOT, check=True)
    failed = False
    for case in ("sparse", "reserve", "efi", "errors", "many", "bootstrap", "exhaust", "capacity",
                 "extended", "conservation", "source_conflict", "mask_4g",
                 "unreachable", "map_collision", "boot_block",
                 "empty_secondary", "zero_framebuffer", "partial_adjacent", "partial_overlap",
                 "partial_gap_reserved", "fragmented_adjacent", "fragmented_overlap",
                 "fragmented_gap", "fragmented_reserved", "fragmented_skip_reserved",
                 "fragmented_efi", "malformed_efi_handoff", "valid_efi_handoff",
                 "mapped_workspace_after_hole", "reserve_acpi_resources"):
        print(f"CASE {case}", flush=True)
        result = subprocess.run([str(binary), case], cwd=ROOT, timeout=15)
        failed |= result.returncode != 0
    if failed:
        raise SystemExit(1)

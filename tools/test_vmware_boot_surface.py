#!/usr/bin/env python3
"""Check real VMware probe keeps the live framebuffer coherent on failure."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-vmware-boot-") as work:
    binary = str(Path(work) / "test")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/vmware_boot_surface_test.c", "-o", binary], cwd=root, check=True)
    failed = []
    for scenario in ("bad_mode", "no_enable_ack", "uc_failure", "alias_failure", "max_width", "max_height",
                     "success", "enabled_alias_failure", "enabled_max_width", "enabled_success"):
        print(f"CASE {scenario}", flush=True)
        result = subprocess.run([binary, scenario], cwd=root, timeout=20)
        if result.returncode:
            failed.append((scenario, result.returncode))
    if failed:
        raise SystemExit(f"Failed VMware probe cases: {failed}")

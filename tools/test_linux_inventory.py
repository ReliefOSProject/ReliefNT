#!/usr/bin/env python3
"""Run kernel CPU, SMBIOS, sysfs and procfs inventory parsers on the host."""
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "out/linux-inventory"
def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], cwd=ROOT, check=True, **kwargs)
def host():
    executable=WORK/'inventory-host'
    run(['cc','-std=c11','-O1','-g','-fsanitize=address,undefined','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
         '-Iinclude','-Iinclude','-Iinclude/uapi',
         '-Iinclude','tools/tests/linux_inventory_test.c','-o',executable])
    run([executable])

if __name__ == "__main__":
    WORK.mkdir(parents=True, exist_ok=True)
    host()

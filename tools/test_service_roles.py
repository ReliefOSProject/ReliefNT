#!/usr/bin/env python3
"""Pin kernel credential role IDs independently of the rootfs group database."""
from pathlib import Path
import re
ROOT = Path(__file__).resolve().parents[1]
KERNEL_USERLAND = ROOT / "kernel/exec/userland.c"
WINDOW_SERVER_GID = 60001
SERVICE_GID = 60002
def check_kernel_defines(failures):
    text = KERNEL_USERLAND.read_text()
    for name, value in (("RELIEFOS_GID_WINDOW_SERVER", WINDOW_SERVER_GID),
                        ("RELIEFOS_GID_SERVICE", SERVICE_GID)):
        match = re.search(rf"^#define {name} (\d+)u$", text, re.M)
        if not match:
            failures.append(f"KERNEL: {name} define missing in {KERNEL_USERLAND}")
        elif int(match.group(1)) != value:
            failures.append(f"KERNEL: {name}={match.group(1)}, expected {value}")


if __name__ == "__main__":
    failures = []
    check_kernel_defines(failures)
    assert not failures, "\n".join(failures)
    print("PASS kernel service credential IDs")

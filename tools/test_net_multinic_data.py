#!/usr/bin/env python3
"""Exercise IPv4 link isolation and AF_PACKET device binding with sanitizers."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp"))).resolve()
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="reliefnt-multinic-data-", dir=temporary) as work:
        for name in ("net_multinic_data", "net_packet"):
            binary = Path(work) / name
            subprocess.run(["cc", "-std=c11", "-O1", "-g", "-DRELIEFNT_HOST_TEST",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Iinclude", "-Iinclude/uapi", f"tools/tests/{name}_test.c",
                            "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    main()

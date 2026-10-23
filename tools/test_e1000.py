#!/usr/bin/env python3
"""Exercise e1000 DMA aliases and the internal driver ABI version gate."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp")))
temporary.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="reliefos-e1000-", dir=temporary) as work:
    for name in ("e1000_init", "driver_module_abi"):
        binary = Path(work) / name
        # The ABI predicate has no allocations. ASan's global-registration
        # constructor would retain unrelated hardware callback tables despite
        # section GC; UBSan covers this isolated predicate without that root.
        sanitizers = "undefined" if name == "driver_module_abi" else "address,undefined"
        subprocess.run(["cc", "-std=c11", "-O1", "-g", f"-fsanitize={sanitizers}",
                        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                        "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                        f"tools/tests/{name}_test.c", "-o", str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)

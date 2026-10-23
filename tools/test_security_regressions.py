#!/usr/bin/env python3
"""Audit kernel ioctl separation and execute production capability checks."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_KERNEL_RE = re.compile(
    r"\b(?:(?:LEONOS|RELIEFOS)_GUI_IOCTL|(?:LEONOS|RELIEFOS)_AUTH_IOCTL|(?:LEONOS|RELIEFOS)_IOCTL_NET_|"
    r"(?:LEONOS|RELIEFOS)_INPUTM_IOCTL|(?:LEONOS|RELIEFOS)_STARTUP_IOCTL|(?:LEONOS|RELIEFOS)_FS_IOCTL_|"
    r"(?:LEONOS|RELIEFOS)_IOCTL_AUDIO_|(?:LEONOS|RELIEFOS)_IOCTL_DEVICE_LIST|(?:LEONOS|RELIEFOS)_IOCTL_DRIVER_|"
    r"(?:LEONOS|RELIEFOS)_TEXT_IOCTL|(?:LEONOS|RELIEFOS)_IOCTL_LIST_DIR|(?:LEONOS|RELIEFOS)_KERNEL_DEBUG_IOCTL)"
    r"[A-Z0-9_]*\b"
)

KERNEL_ROOTS = tuple(name for name in ("arch", "block", "drivers", "firmware", "fs", "kernel", "lib", "mm", "net", "sound"))
def kernel_violations() -> list[str]:
    violations: list[str] = []
    for root in KERNEL_ROOTS:
        for path in sorted((ROOT / root).rglob("*")):
            if path.suffix not in {".c", ".h"}:
                continue
            text = path.read_text(encoding="utf-8")
            for match in FORBIDDEN_KERNEL_RE.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                violations.append(f"{path.relative_to(ROOT)}:{line}: {match.group(0)}")
    return violations

def credential_regressions() -> list[str]:
    # Linux kernel/sys.c and kernel/reboot.c use separate capabilities. The
    # historical source-string assertion required incorrect real-UID gates.
    result = subprocess.run([sys.executable, "tools/test_linux_capabilities.py"],
                            cwd=ROOT, capture_output=True, text=True, timeout=60)
    if result.returncode:
        return ["production credential regression failed: " + (result.stdout + result.stderr)[-4000:]]
    return []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()
    violations = kernel_violations()
    credentials = credential_regressions()
    result = {"kernel_private_ioctl_violations": violations,
              "credential_regression_failures": credentials}
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(json.dumps(result, indent=2))
        print("PASS kernel security boundaries" if not (violations or credentials) else "FAIL kernel security boundaries")
    return int(bool(violations or credentials))
if __name__ == "__main__":
    sys.exit(main())

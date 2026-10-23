#!/usr/bin/env python3
"""Reject retired desktop implementations and executable-GID service authority."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def main():
    for name in ("drivers/video/framebuffer/desktop.c",
                 "drivers/video/framebuffer/desktop_input.c"):
        assert not (ROOT / name).exists(), f"retired desktop implementation is present: {name}"
    print("PASS retired kernel desktop implementations are absent")
    source = (ROOT / "kernel/exec/userland.c").read_text()
    types = (ROOT / "include/reliefnt/task/types.h").read_text()
    assert not re.search(r"RELIEFOS_GID_(?:WINDOW_SERVER|SERVICE)|TASK_FLAG_WINDOW_SERVER",
                         source + types), "retired window/service authority is present"
    # exec may preserve existing credentials; an executable's group must not
    # introduce the kernel-only service flag into the new image's task flags.
    match = re.search(r"preserved_flags\s*=\s*task->flags\s*&\s*\(([^;]+)\);", source)
    assert match is not None, "exec flag preservation contract is missing"
    flags = {flag.strip() for flag in match.group(1).split("|")}
    assert flags == {"TASK_FLAG_ELEVATED_ADMIN", "TASK_FLAG_WAITABLE_CHILD"}, flags
    print("PASS exec does not grant window/service authority through executable GIDs")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Check canonical and rollback boot identities accepted by the kernel."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
assert '"reliefos-installer-root"' in (ROOT / 'fs/vfs/mount.c').read_text(), 'kernel accepts the canonical installer root tag'
assert '"leonos-installer-root"' in (ROOT / 'fs/vfs/mount.c').read_text(), 'kernel accepts the legacy installer root tag'
assert 'KERNEL_DEBUG_MARKER "RELIEFOS-KDBG-1\\n"' in (ROOT / 'kernel/init/kernel_debug.c').read_text(), 'kernel writes the canonical one-shot marker'
assert 'RELIEFOS_PATH_BOOT_KERNELDEBUG_MARKER "/boot/reliefos/state/kerneldebug.next"' in (ROOT / 'include/reliefos/layout.h').read_text(), 'kernel reads the canonical one-shot marker'
assert '"/boot/leonos/state/kerneldebug.next"' in (ROOT / 'kernel/init/kernel_debug.c').read_text(), 'kernel reads the legacy one-shot marker'
assert '"RELIEFOS_ROOT"' in (ROOT / 'block/core/storage_block.c').read_text(), 'kernel recognizes the canonical GPT root name'
assert '"LEONOS4_ROOT"' in (ROOT / 'block/core/storage_block.c').read_text(), 'kernel recognizes the legacy GPT root name'
assert "kernel_name=ReliefNT" in (ROOT / "configs/build-version").read_text()
assert '#define LINUX_UTS_SYSNAME "ReliefNT"' in (ROOT / "include/reliefnt/uts.h").read_text()
print("PASS kernel boot identity, root tags and rollback markers")

#!/bin/sh
# The standalone kernel must never source the parent product configuration.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/out"
cat > "$work/out/.config" <<'CONFIG'
CONFIG_RELIEFOS_COMPONENT_APP_FOREIGN_BUILD=y
CONFIG_VMDK_REQUIRE_LICENSE=y
CONFIG_QEMU_SMP_CPUS=32
CONFIG
make -s -C "$src" O="$work/out" olddefconfig
make -s -C "$src" O="$work/out" config-sync
# Only symbols declared by the kernel's own Kconfig tree may be generated.
# This continues to reject product/application configuration as native kernel
# module options are introduced; tests and the imported Kconfig engine are not
# part of the production configuration declaration inventory.
python3 - "$src" "$work/out" <<'PY'
from pathlib import Path
import re
import sys
src, out = map(Path, sys.argv[1:])
paths = [src / 'Kconfig']
seen = set()
allowed = set()
while paths:
    path = paths.pop().resolve()
    if path in seen:
        continue
    if not path.is_relative_to(src):
        raise SystemExit('kernel Kconfig sources a foreign tree: ' + str(path))
    seen.add(path)
    content = path.read_text()
    allowed.update('CONFIG_' + name for name in re.findall(r'^\s*(?:config|menuconfig)\s+([A-Z0-9_]+)', content, re.M))
    paths.extend(src / name for name in re.findall(r'^\s*source\s+"([^"$]+)"', content, re.M))
for path in (out / '.config', out / 'include/generated/autoconf.h'):
    actual = set(re.findall(r'\bCONFIG_[A-Z0-9_]+', path.read_text()))
    foreign = actual - allowed
    if foreign:
        raise SystemExit('foreign kernel configuration: ' + ', '.join(sorted(foreign)))
assert 'CONFIG_MODULES=y' in (out / '.config').read_text()
PY
cp "$work/out/.config" "$work/before"
make -s -C "$src" O="$work/out" olddefconfig
cmp "$work/before" "$work/out/.config"
printf 'ReliefNT standalone configuration boundary passed\n'

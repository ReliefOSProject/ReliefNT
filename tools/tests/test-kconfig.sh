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
# This kernel currently has no optional features. Its generated configuration
# must therefore contain no product, application or invented kernel options.
if grep -E '^(CONFIG_|# CONFIG_)' "$work/out/.config" ||
   grep -E '^#define CONFIG_' "$work/out/include/generated/autoconf.h"; then
    echo 'ReliefNT generated configuration contains foreign options' >&2
    exit 1
fi
cp "$work/out/.config" "$work/before"
make -s -C "$src" O="$work/out" olddefconfig
cmp "$work/before" "$work/out/.config"
printf 'ReliefNT standalone configuration boundary passed\n'

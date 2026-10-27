#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Content identity only: this inventory never supplies compilation members.
set -eu
[ "$#" = 5 ] || { echo 'usage: module-abi SRC O FLAGS EXTRA LOCAL' >&2; exit 2; }
src=$(CDPATH= cd -- "$1" && pwd -P)
mkdir -p "$2"
out=$(CDPATH= cd -- "$2" && pwd -P)
flags=$3 extra=$4 local=$5
case "$extra$local" in *[!a-zA-Z0-9._+-]*) echo 'unsafe kernel release suffix' >&2; exit 1 ;; esac
version=$(sed -n 's/^release_version=//p' "$src/configs/build-version")
printf '%s\n' "$version" | awk 'BEGIN { ok=0 } /^[0-9]+\.[0-9]+\.[0-9]+$/ { ok++ } END { exit !(NR==1 && ok==1) }' || {
    echo 'invalid native kernel release version' >&2; exit 1;
}
[ -f "$out/.config" ] || {
    echo 'module identity requires synchronized configuration and compiler identity' >&2; exit 1;
}
for tool in cc ar ld objcopy; do
    [ -f "$out/.reliefnt-target-$tool.id" ] || { echo "missing module toolchain identity: $tool" >&2; exit 1; }
done
work=$(mktemp -d "$out/.module-abi.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
(
    cd "$src"
    find arch block drivers firmware fs kernel lib mm net sound vdso include scripts \
        \( -type f -o -type l \) \
        \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.ld' -o -name '*.lds' \
           -o -name 'Kbuild' -o -name 'Kconfig' -o -name '*.sh' -o -name 'Makefile*' \) -print
    if [ -d tools/host ]; then
        find tools/host \( -type f -o -type l \) \
            \( -name '*.c' -o -name '*.h' -o -name Kbuild \) -print
    fi
    if [ -d tools/tests/kmodules ]; then
        find tools/tests/kmodules -type f \( -name '*.c' -o -name '*.h' -o -name Kbuild -o -name Kconfig \) -print
    fi
    printf '%s\n' Kbuild Kconfig Makefile configs/build-version scripts/kbuild-source.json
    if [ -f configs/dependencies.lock.json ]; then printf '%s\n' configs/dependencies.lock.json; fi
) | LC_ALL=C sort -u > "$work/inputs"
(
    cd "$src"
    while IFS= read -r input; do
        case "$src/$input" in "$out"|"$out"/*) continue ;; esac
        sha256sum -- "$input"
    done < "$work/inputs"
    printf 'native-module-abi=1\npolicy=%s\nextra=%s\nlocal=%s\n' "$flags" "$extra" "$local"
    for tool in cc ar ld objcopy; do
        printf 'tool=%s\n' "$tool"
        cat "$out/.reliefnt-target-$tool.id"
    done
    awk '/^CONFIG_[A-Z0-9_]+=/ || /^# CONFIG_[A-Z0-9_]+ is not set$/ { print }' "$out/.config" | LC_ALL=C sort
) > "$work/content"
identity=$(sha256sum "$work/content" | cut -d' ' -f1)
short=$(printf '%.12s' "$identity")
release=$version$extra$local-ko1-$short
[ "${#release}" -le 31 ] || { echo 'kernel release with native ABI suffix exceeds 31 bytes' >&2; exit 1; }
cat > "$work/module_abi.h" <<HEADER
/* Generated native module identity; no clock or workspace path is included. */
#ifndef RELIEFNT_GENERATED_MODULE_ABI_H
#define RELIEFNT_GENERATED_MODULE_ABI_H
#define RELIEFNT_KERNEL_RELEASE "$release"
#define RELIEFNT_MODULE_BUILD_ID "$identity"
#define RELIEFNT_MODULE_VERMAGIC RELIEFNT_KERNEL_RELEASE " ReliefNT-1 " RELIEFNT_MODULE_BUILD_ID
#endif
HEADER
printf '%s\n' "$release" > "$work/kernel.release"
printf '%s\n' "$identity" > "$work/module.abi"
mkdir -p "$out/include/generated" "$out/include/config"
for item in module_abi.h kernel.release module.abi; do
    case "$item" in module_abi.h) destination=$out/include/generated/$item ;; *) destination=$out/include/config/$item ;; esac
    if ! cmp -s "$work/$item" "$destination"; then mv -f "$work/$item" "$destination"; fi
done

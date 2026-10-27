#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Original modinst is allowed to remove directories only in this private stage.
set -eu
[ "$#" -ge 6 ] || { echo 'usage: modules-stage SRC O PREFIX RELEASE EXTMOD MAKE...' >&2; exit 2; }
src=$1 out=$2 prefix=$3 release=$4 extmod=$5
shift 5
# Recursive make recipes also run under -n; do not parse destructive upstream
# modinst rules or even create a stage in that case.
first_flags=${MAKEFLAGS%% *}
case "$first_flags" in --*) ;; *n*|*q*) exit 0 ;; esac
work=$(mktemp -d "${RELIEFNT_MODINST_TMP:-$out}/.modinst.XXXXXX")
trap 'rm -rf "$work"' 0 HUP INT TERM
mkdir -p "$work/lib/modules/$release"
"$@" MODLIB="$work/lib/modules/$release" INSTALL_MOD_PATH="$work" \
    KERNELRELEASE="$release" DEPMOD=true
if [ -z "$extmod" ]; then
    owner=kernel
    cp "$out/Module.symvers" "$work/lib/modules/$release/Module.symvers"
    cp "$out/System.map" "$work/lib/modules/$release/System.map"
    cp "$out/.config" "$work/lib/modules/$release/config"
    cp "$out/include/config/module.abi" "$work/lib/modules/$release/module.abi"
    # Original modinst omits this file when MODULES=n. The all-builtin bundle
    # still needs an explicit empty module set; no imported rule is changed.
    if [ ! -f "$work/lib/modules/$release/modules.order" ]; then
        [ ! -s "$out/modules.order" ] || { echo 'disabled modules have a nonempty order' >&2; exit 1; }
        : > "$work/lib/modules/$release/modules.order"
    fi
else
    owner=external-$(printf '%s' "$extmod" | sha256sum | cut -c1-20)
fi
sh "$src/scripts/modules-publish.sh" "$work" "$prefix" "$release" "$owner"
# depmod's caches describe the final tree, including retained external/user
# modules. They are generated indexes, not install ownership manifests.
if grep -q '^CONFIG_MODULES=y$' "$out/.config"; then
    command -v "${DEPMOD:-depmod}" >/dev/null 2>&1 || {
        echo 'modules_install requires host depmod (kmod)' >&2; exit 1;
    }
    "${DEPMOD:-depmod}" -b "$prefix" "$release"
fi

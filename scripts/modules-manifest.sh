#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# A product list, never a compilation source list. Paths are relative to O.
set -eu
[ "$#" = 1 ] || exit 2
cd "$1"
manifest=modules.manifest
trap 'rm -f "$manifest.tmp"' 0 HUP INT TERM
{
    printf '# ReliefNT module bundle version=1 release=%s\n' "$(cat include/config/kernel.release)"
    sha256sum .config include/config/module.abi include/config/kernel.release \
        include/generated/module_abi.h Module.symvers System.map \
        modules.order modules.builtin modules.builtin.modinfo generated/system/kernel.sys
    while IFS= read -r object; do
        case "$object" in ''|/*|*[!a-zA-Z0-9_./+-]*|../*|*/../*) exit 1 ;; esac
        sha256sum "${object%.o}.ko"
    done < modules.order
} > "$manifest.tmp"
if cmp -s "$manifest.tmp" "$manifest"; then rm -f "$manifest.tmp"; else mv -f "$manifest.tmp" "$manifest"; fi

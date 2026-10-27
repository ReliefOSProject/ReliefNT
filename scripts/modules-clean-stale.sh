#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Retire only unchanged, previously generated module products in this output.
set -eu
[ "$#" = 2 ] || exit 2
root=$(realpath -e -- "$1")
order=$2
manifest=$root/.reliefnt-module-products.manifest
work=$(mktemp -d "$root/.module-products.XXXXXX")
trap 'rm -rf "$work"' 0 HUP INT TERM
: > "$work/new"
while IFS= read -r object; do
    case "$object" in "$root"/*) object=${object#"$root"/} ;; esac
    object=${object#./}
    case "$object" in ''|/*|*[!a-zA-Z0-9_./+-]*|../*|*/../*|*/./*) echo 'invalid modules.order member' >&2; exit 1 ;; esac
    base=${object%.o}
    for suffix in .ko .mod .mod.c .mod.o; do
        relative=$base$suffix
        if [ -f "$root/$relative" ] && [ ! -L "$root/$relative" ]; then
            checksum=$(sha256sum "$root/$relative" | cut -d' ' -f1)
            printf '%s  %s\n' "$checksum" "$relative" >> "$work/new"
        fi
    done
done < "$order"
if [ -f "$manifest" ] && [ ! -L "$manifest" ]; then
    while read -r checksum relative; do
        case "$relative" in ''|/*|*[!a-zA-Z0-9_./+-]*|../*|*/../*|*/./*) exit 1 ;; esac
        case "$(realpath -m -- "$root/$relative")" in "$root"/*) ;; *) exit 1 ;; esac
        if ! awk -v path="$relative" '$2 == path { found=1 } END { exit !found }' "$work/new"; then
            if [ -f "$root/$relative" ] && [ ! -L "$root/$relative" ] &&
               [ "$(sha256sum "$root/$relative" | cut -d' ' -f1)" = "$checksum" ]; then
                rm -f "$root/$relative"
                # Kbuild command records describe exactly that generated file.
                rm -f "$root/$(dirname "$relative")/.$(basename "$relative").cmd"
            fi
        fi
    done < "$manifest"
fi
mv -f "$work/new" "$manifest"

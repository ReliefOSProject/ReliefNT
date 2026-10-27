#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Publish a private modinst tree without deleting user modules or old releases.
set -eu
[ "$#" = 4 ] || { echo 'usage: modules-publish STAGE PREFIX RELEASE OWNER' >&2; exit 2; }
stage=$1 prefix=$2 release=$3 owner=$4
die() { printf 'modules-publish: %s\n' "$*" >&2; exit 1; }
case "$release" in ''|.|..|*[!a-zA-Z0-9._+-]*) die 'invalid release' ;; esac
[ "${#release}" -le 31 ] || die 'release exceeds native utsname capacity'
case "$owner" in ''|.|..|*[!a-zA-Z0-9._+-]*) die 'invalid owner' ;; esac
prefix=$(realpath -m -- "$prefix")
[ "$prefix" != / ] || die 'an explicit private installation prefix is required'
destination=$(realpath -m -- "$prefix/lib/modules/$release")
case "$destination" in "$prefix"/*) ;; *) die 'installation symlink escapes prefix' ;; esac
source=$(realpath -e -- "$stage/lib/modules/$release")
[ -d "$source" ] || die 'missing private stage'
mkdir -p "$destination"
[ ! -L "$destination/.reliefnt-install.lock" ] || die 'symlink lock'
exec 9>"$destination/.reliefnt-install.lock"
flock -n 9 || die 'this release is being installed by another process'
work=$(mktemp -d "$destination/.publish.XXXXXX")
trap 'rm -rf "$work"' 0 HUP INT TERM
manifest=$destination/.reliefnt-$owner.manifest
[ ! -L "$manifest" ] || die 'symlink manifest'
old=$work/old
if [ -f "$manifest" ]; then cp "$manifest" "$old"; else : > "$old"; fi

valid_path() {
    case "$1" in ''|/*|*[!a-zA-Z0-9_./+-]*|.|..|./*|../*|*/./*|*/../*|*/.|*/..|*//*|.reliefnt*|.publish*) return 1 ;; esac
}
contained_path() {
    valid_path "$1" || die "invalid managed path: $1"
    case "$(realpath -m -- "$destination/$1")" in "$destination"/*) ;; *) die "path escapes release: $1" ;; esac
    [ ! -L "$destination/$1" ] || die "symlink destination: $1"
}
digest() { sha256sum -- "$1" | cut -d' ' -f1; }
while read -r checksum relative extra; do
    [ -n "$checksum" ] || continue
    [ -z "$extra" ] && [ "${#checksum}" = 64 ] || die 'invalid previous ownership manifest'
    case "$checksum" in *[!0-9a-f]*) die 'invalid manifest checksum' ;; esac
    contained_path "$relative"
done < "$old"

# The upstream installer creates a host build symlink. It is deliberately not
# part of a deployable module tree; no other stage symlinks are accepted.
find "$source" -type l ! -path "$source/build" -print > "$work/symlinks"
[ ! -s "$work/symlinks" ] || die 'symlink in private module payload'
(cd "$source" && find . -type f -print | LC_ALL=C sort) > "$work/files"
: > "$work/new"
mkdir "$work/payload"
while IFS= read -r entry; do
    relative=${entry#./}
    contained_path "$relative"
    checksum=$(digest "$source/$relative")
    previous=$(awk -v path="$relative" '$2 == path { print $1 }' "$old")
    if [ -e "$destination/$relative" ]; then
        [ -f "$destination/$relative" ] && [ -n "$previous" ] || die "unowned destination: $relative"
        [ "$(digest "$destination/$relative")" = "$previous" ] || die "user modified destination: $relative"
    fi
    mkdir -p "$work/payload/$(dirname "$relative")"
    cp "$source/$relative" "$work/payload/$relative"
    [ "$(digest "$work/payload/$relative")" = "$checksum" ] || die "stage changed while copying: $relative"
    printf '%s  %s\n' "$checksum" "$relative" >> "$work/new"
done < "$work/files"

# All collisions are checked before the first visible payload change. Each
# file and the final ownership list are published by rename on the same FS.
while read -r checksum relative; do
    contained_path "$relative"
    mkdir -p "$destination/$(dirname "$relative")"
    mv -f "$work/payload/$relative" "$destination/$relative"
done < "$work/new"
while read -r checksum relative; do
    [ -n "$checksum" ] || continue
    if ! awk -v path="$relative" '$2 == path { found=1 } END { exit !found }' "$work/new"; then
        contained_path "$relative"
        if [ -f "$destination/$relative" ] && [ "$(digest "$destination/$relative")" = "$checksum" ]; then
            rm -f "$destination/$relative"
        fi
    fi
done < "$old"
mv -f "$work/new" "$manifest"

#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Export a versioned, relocatable prepared tree, never a host build symlink.
set -eu
[ "$#" = 3 ] || { echo 'usage: modules-sdk SRC O PREFIX' >&2; exit 2; }
src=$(realpath -e -- "$1") out=$(realpath -e -- "$2") prefix=$(realpath -m -- "$3")
die() { printf 'modules-sdk: %s\n' "$*" >&2; exit 1; }
[ "$prefix" != / ] || die 'an explicit private installation prefix is required'
case "$prefix" in "$src"|"$src/include"|"$src/include/"*|"$src/scripts"|"$src/scripts/"*|"$src/LICENSES"|"$src/LICENSES/"*) die 'SDK prefix overlaps source inputs' ;; esac
grep -q '^CONFIG_MODULES=y$' "$out/.config" || die 'prepared module SDK requires CONFIG_MODULES=y'
release=$(cat "$out/include/config/kernel.release")
case "$release" in ''|.|..|*[!a-zA-Z0-9._+-]*) die 'invalid release' ;; esac
[ "${#release}" -le 31 ] || die 'release exceeds native utsname capacity'
raw_destination=$prefix/usr/src/reliefnt-$release
[ ! -L "$raw_destination" ] || die 'symlink SDK destination'
destination=$(realpath -m -- "$raw_destination")
library=$(realpath -m -- "$prefix/lib/modules/$release")
case "$destination" in "$prefix"/*) ;; *) die 'SDK directory escapes prefix' ;; esac
case "$library" in "$prefix"/*) ;; *) die 'module directory escapes prefix' ;; esac
build_link=$(realpath -m --relative-to="$library" "$destination")
if [ -L "$library/build" ]; then
    [ "$(readlink "$library/build")" = "$build_link" ] || die 'unowned module build link'
elif [ -e "$library/build" ]; then die 'unowned module build destination'; fi
mkdir -p "$prefix/usr/src" "$library"
lock=$prefix/usr/src/.reliefnt-sdk-$release.lock
[ ! -L "$lock" ] || die 'symlink SDK lock'
exec 9>"$lock";flock -n 9 || die 'this SDK is being published by another process'
if [ -e "$destination" ]; then
    [ -d "$destination" ] && [ ! -L "$destination" ] &&
        [ -f "$destination/.reliefnt-sdk-manifest" ] || die 'unowned prepared SDK destination'
    (cd "$destination" && sha256sum -c .reliefnt-sdk-manifest >/dev/null) || die 'user modified prepared SDK'
    [ "$(cat "$destination/include/config/module.abi")" = "$(cat "$out/include/config/module.abi")" ] || die 'prepared SDK ABI collision'
    [ -L "$library/build" ] || ln -s "$build_link" "$library/build"
    exit 0
fi
work=$(mktemp -d "$prefix/usr/src/.reliefnt-sdk.XXXXXX")
trap 'rm -rf "$work"' 0 HUP INT TERM
find "$src/include" "$src/scripts" "$src/LICENSES" -type l -print > "$work/symlinks"
[ ! -s "$work/symlinks" ] || die 'symlink in SDK source inventory'
rm "$work/symlinks"
cp -R "$src/include" "$src/scripts" "$src/LICENSES" "$work/"
cp "$src/LICENSE" "$work/LICENSE"
cp "$src/scripts/Makefile.module-sdk" "$work/Makefile"
mkdir -p "$work/include/generated" "$work/include/config" "$work/scripts/basic" "$work/scripts/mod" "$work/scripts/genksyms"
cp "$out/.config" "$out/Module.symvers" "$work/"
for file in autoconf.h module_abi.h host-toolchain-id.h; do cp "$out/include/generated/$file" "$work/include/generated/$file"; done
for file in auto.conf kernel.release module.abi; do cp "$out/include/config/$file" "$work/include/config/$file"; done
for tool in cc ar ld objcopy; do cp "$out/.reliefnt-target-$tool.id" "$work/"; done
for binary in scripts/basic/fixdep scripts/mod/modpost scripts/genksyms/genksyms scripts/module.lds; do
    cp "$out/$binary" "$work/$binary"
done
emit() {
    key=$1 value=$2
    case "$value" in *'
'*) die 'newline in SDK policy' ;; esac
    value=$(printf '%s' "$value" | sed -e 's/\$/$$/g' -e 's/#/\\#/g')
    printf '%s := %s\n' "$key" "$value"
}
portable_tool() {
    key=$1 command=$2
    set -- $command
    first=${1##*/};shift
    rest=
    if [ "$#" -gt 0 ]; then rest=$(printf ' %s' "$@"); fi
    emit "$key" "$first$rest"
}
{
    emit SDK_ARCH "$SDK_ARCH";emit SDK_PROFILE "$SDK_PROFILE";emit SDK_TRIPLE "$SDK_TRIPLE"
    portable_tool SDK_CC "$SDK_CC";portable_tool SDK_AR "$SDK_AR";portable_tool SDK_LD "$SDK_LD"
    portable_tool SDK_OBJCOPY "$SDK_OBJCOPY";portable_tool SDK_NM "$SDK_NM";portable_tool SDK_STRIP "$SDK_STRIP"
    emit SDK_CFLAGS "$SDK_CFLAGS";emit SDK_AFLAGS "$SDK_AFLAGS";emit SDK_LDFLAGS "$SDK_LDFLAGS"
    emit SDK_SOURCE_ID "$SDK_SOURCE_ID"
} > "$work/sdk-config.mk"
(cd "$work" && find . -type f ! -name .reliefnt-sdk-manifest -print | LC_ALL=C sort |
    while IFS= read -r file; do sha256sum -- "$file"; done) > "$work/.reliefnt-sdk-manifest"
mv "$work" "$destination"
[ -L "$library/build" ] || ln -s "$build_link" "$library/build"
printf 'Prepared native module SDK: %s\n' "$destination"

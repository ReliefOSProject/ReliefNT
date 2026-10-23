#!/bin/sh
# Standalone version renderer and kernel procfs host regression.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
kernel=$src
kernel_release=$(sed -n 's/^release_version=//p' "$kernel/configs/build-version")
kernel_major=${kernel_release%%.*}
kernel_minor_patch=${kernel_release#*.}
kernel_minor=${kernel_minor_patch%%.*}
kernel_patch=${kernel_minor_patch#*.}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -I"$kernel" \
    "$kernel/tools/host/version/reliefos-version.c" \
    "$kernel/tools/host/common/io.c" "$kernel/tools/host/common/buffer.c" -o "$work/version"
render() {
    "$work/version" --version-file "$kernel/configs/build-version" \
        --source-id abc123 --epoch 1700000000 --output "$work/build.h" "$@"
}
render
grep -Fqx "#define RELIEFOS_KERNEL_VERSION \"$kernel_release\"" "$work/build.h"
render --extra-version -perf --local-version -test
grep -Fqx "#define RELIEFOS_KERNEL_VERSION \"$kernel_release-perf-test\"" "$work/build.h"
grep -Fqx "#define RELIEFOS_KERNEL_VERSION_MAJOR $kernel_major" "$work/build.h"
grep -Fqx "#define RELIEFOS_KERNEL_VERSION_MINOR $kernel_minor" "$work/build.h"
grep -Fqx "#define RELIEFOS_KERNEL_VERSION_PATCH $kernel_patch" "$work/build.h"
before=$(stat -c %y "$work/build.h")
render --extra-version -perf --local-version -test
test "$before" = "$(stat -c %y "$work/build.h")"
cp "$work/build.h" "$work/expected.h"
for suffix in 'bad/flag' 'bad"flag' 'abcdefghijklmnopqrstuvwxyz0123456789'; do
    if render --extra-version "$suffix" >"$work/rejected.log" 2>&1; then
        echo "FAIL - unsafe or oversized kernel suffix accepted: $suffix"
        exit 1
    fi
    cmp "$work/expected.h" "$work/build.h"
done
render --extra-version '' --local-version ''
grep -Fqx "#define RELIEFOS_KERNEL_VERSION \"$kernel_release\"" "$work/build.h"
${HOSTCC:-cc} -std=c11 -O1 -g -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$kernel/include" -I"$kernel/include/uapi" \
    "$kernel/tools/tests/procfs_directories_test.c" -o "$work/procfs"
"$work/procfs"
printf 'kernel version renderer: base, suffixes, ABI, no-op, reset, validation and procfs passed\n'

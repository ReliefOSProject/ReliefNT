#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Portable effective tool identity, shared by kernel builds and prepared SDKs.
set -eu
[ "$#" -gt 0 ] || { echo 'usage: tool-identity TOOL [ARG...]' >&2; exit 2; }
mode=${1##*/}
executable=$(command -v "$1")
version=$("$@" --version 2>&1 | sed -n '1p')
checksum=$(sha256sum -- "$executable" | cut -d' ' -f1)
shift
arguments=
if [ "$#" -gt 0 ]; then arguments=$(printf ' %s' "$@"); fi
printf '%s\n%s\n%s\n%s\n' "$mode" "$arguments" "$version" "$checksum" | sha256sum | cut -d' ' -f1

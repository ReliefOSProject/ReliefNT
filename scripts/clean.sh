#!/bin/sh
# Remove regenerable build products from the selected output directory.
#
# Safety rules from the migration plan section 6.3: never `rm -rf $(O)` blindly,
# require an ownership marker, refuse the source root and the filesystem root,
# and keep the shared download cache in every mode.
set -u

target=${O:-}
src=${SRC:-}
keep=${KEEP_CONFIG:-1}

refuse() {
    printf 'clean: refusing to clean: %s\n' "$1" >&2
    exit 1
}

case "$target" in
    /*) ;;
    *)  case "$target" in
            "") refuse 'O is empty' ;;
            *)  target="$PWD/$target" ;;
        esac ;;
esac

[ "$target" = "/" ] && refuse 'O is the filesystem root'
[ -n "$src" ] && [ "$target" = "$src" ] && refuse "O is the source root ($src)"

# Resolve all components before reading the marker or removing products.
resolved=$(realpath -ms -- "$target" 2>/dev/null) || refuse 'cannot resolve O'
physical=$(realpath -m -- "$target" 2>/dev/null) || refuse 'cannot resolve O'
[ "$physical" = "$resolved" ] || refuse 'O contains a symlink component'
[ "$resolved" != / ] || refuse 'resolved O is the filesystem root'
if [ -n "$src" ]; then
    source_root=$(realpath -m -- "$src") || refuse 'cannot resolve source root'
    [ "$resolved" != "$source_root" ] || refuse 'resolved O is the source root'
fi
target=$resolved
marker="$target/.reliefos-out"
[ ! -L "$marker" ] || refuse 'ownership marker is a symlink'
if [ ! -f "$marker" ]; then
    refuse "no ownership marker at $marker (not a build output directory created by this Makefile)"
fi
if ! grep -q '^reliefos-build-out version=1 ' "$marker"; then
    refuse "ownership marker in $target is not a ReliefOS build output marker"
fi
marker_root=$(sed -n 's/^reliefos-build-out version=1 root=//p' "$marker" | head -n1)
if [ -n "$src" ] && [ -n "$marker_root" ] && [ "$marker_root" != "$src" ]; then
    refuse "output directory belongs to a different source root ($marker_root)"
fi

# Explicit list rather than a glob: an unknown entry in the tree is not ours to
# delete, and this is what makes `clean` auditable. These are the source-relative
# object directories produced by native Kbuild plus ReliefNT's generated files.
products="arch block debug drivers firmware fs generated host include kernel kernel-export kernel-install lib mm net scripts sound tools vdso"
if [ "$keep" = 0 ]; then
	for config_file in .config .config.old .config.old.1 .tmp.config; do
		if [ -e "$resolved/$config_file" ] || [ -L "$resolved/$config_file" ]; then
			printf '  remove %s\n' "$config_file"
			rm -f -- "$resolved/$config_file" || exit 1
		fi
	done
fi

printf 'clean: %s (keep config=%s)\n' "$resolved" "$keep"
for entry in $products; do
    if [ -e "$resolved/$entry" ] || [ -L "$resolved/$entry" ]; then
        printf '  remove %s\n' "$entry"
        rm -rf -- "$resolved/$entry" || exit 1
    fi
done
for generated_file in built-in.a .built-in.a.cmd \
	.reliefnt-target-cc.id .reliefnt-target-ar.id .reliefnt-target-ld.id .reliefnt-target-objcopy.id; do
	if [ -e "$resolved/$generated_file" ] || [ -L "$resolved/$generated_file" ]; then
		printf '  remove %s\n' "$generated_file"
		rm -f -- "$resolved/$generated_file" || exit 1
	fi
done
if [ "$keep" = 0 ]; then
    for marker_entry in .reliefos-out; do
        if [ -e "$resolved/$marker_entry" ] || [ -L "$resolved/$marker_entry" ]; then
            printf '  remove %s\n' "$marker_entry"
            rm -f -- "$resolved/$marker_entry"
        fi
    done
fi
printf 'clean: the shared download cache under cache/downloads was not touched\n'

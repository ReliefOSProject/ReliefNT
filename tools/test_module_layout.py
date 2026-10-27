#!/usr/bin/env python3
"""Audit real per-directory Kbuild ownership and production source coverage."""
from pathlib import Path
import hashlib
import json
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PRODUCTION = ('arch', 'block', 'drivers', 'firmware', 'fs', 'kernel', 'lib', 'mm', 'net', 'sound', 'debug', 'vdso')


def main():
    errors, objects, visited = [], [], set()
    inputs = set()
    generated_sources = set()

    def visit(directory):
        if directory in visited:
            errors.append(f'duplicate directory: {directory}')
            return
        visited.add(directory)
        path = ROOT / directory / 'Kbuild'
        if not path.is_file():
            errors.append(f'missing Kbuild: {directory}')
            return
        inputs.add(path.relative_to(ROOT).as_posix())
        content = path.read_text().replace('\\\n', ' ')
        lists = {}
        included_objects = set()
        # Configuration selects the actual build. This inventory deliberately
        # covers every eligible branch, including currently disabled options.
        assignment = r'^([A-Za-z0-9_.-]+-(?:y|m|objs|\$\(CONFIG_[A-Z0-9_]+\)))\s*([:+]?=)\s*(.*?)\s*(?:#.*)?$'
        for line in content.splitlines():
            child = re.fullmatch(r'include (?:\$\(srctree\)/)?\$\(src\)/([A-Za-z0-9_-]+)/Kbuild', line)
            if child:
                child_directory = directory / child[1]
                child_path = ROOT / child_directory / 'Kbuild'
                if child_directory in visited or not child_path.is_file():
                    errors.append(f'invalid/duplicate included child: {child_directory}')
                    continue
                visited.add(child_directory)
                inputs.add(child_path.relative_to(ROOT).as_posix())
                for child_line in child_path.read_text().replace('\\\n', ' ').splitlines():
                    item = re.match(assignment, child_line)
                    if not item:
                        if child_line.strip() and not child_line.lstrip().startswith('#'):
                            errors.append(f'unsupported child member expression: {child_directory}: {child_line}')
                        continue
                    key, operation, value = item.groups()
                    entries = value.split()
                    if key.startswith(('obj-', 'always-')) or any('/' in entry or not entry.endswith('.o') for entry in entries):
                        errors.append(f'included child must own only local composite members: {child_directory}')
                        continue
                    if operation in ('=', ':='):
                        lists[key] = []
                    lists.setdefault(key, []).extend(entries)
                    included_objects.update(child[1] + '/' + entry for entry in entries)
                continue
            match = re.match(assignment, line)
            if not match:
                continue
            key, operation, value = match.groups()
            # Standard Kbuild may assemble a composite from a child-owned local
            # list. Accept only explicitly included members, never a root list
            # of source paths or a filesystem inventory used for compilation.
            prefix = re.fullmatch(r'\$\(addprefix ([A-Za-z0-9_-]+/),\$\(([A-Za-z0-9_.-]+)\)\)', value)
            if prefix:
                entries = [prefix[1] + entry for entry in lists.get(prefix[2], [])]
                if not entries or any(entry not in included_objects for entry in entries):
                    errors.append(f'composite references unowned child members: {value}')
                value = ' '.join(entries)
            if operation in ('=', ':='):
                lists[key] = []
            lists.setdefault(key, []).extend(value.split())

        def members(stem):
            return [entry for key, entries in lists.items()
                    if key in (stem + '-y', stem + '-m', stem + '-objs')
                    or key.startswith(stem + '-$(CONFIG_') for entry in entries]

        def object_entry(entry, ancestry=()):
            if entry in included_objects:
                objects.append((directory / entry).as_posix().removeprefix('./'))
                return
            if '/' in entry or not entry.endswith('.o'):
                errors.append(f'{path.relative_to(ROOT)}: non-local/invalid object {entry}')
                return
            stem = entry[:-2]
            if stem in ancestry:
                errors.append(f'{path.relative_to(ROOT)}: recursive composite {entry}')
                return
            children = members(stem)
            if children:
                for child in children:
                    object_entry(child, (*ancestry, stem))
            else:
                objects.append((directory / entry).as_posix().removeprefix('./'))

        for key, entries in lists.items():
            if not key.startswith(('obj-', 'always-')):
                continue
            for entry in entries:
                if entry.endswith('/'):
                    if '/' in entry[:-1] or entry.startswith('.'):
                        errors.append(f'{path.relative_to(ROOT)}: non-local subdirectory {entry}')
                    else:
                        visit(directory / entry)
                elif entry.endswith('.o'):
                    object_entry(entry)
                elif key.startswith('always-') and entry.endswith('.lds'):
                    linker = ROOT / directory / (entry + '.S')
                    if not linker.is_file():
                        errors.append(f'{path.relative_to(ROOT)}: missing linker source {entry}.S')
                    else:
                        generated_sources.add(linker.relative_to(ROOT).as_posix())

    visit(Path('.'))
    visit(Path('debug'))
    if len(objects) != len(set(objects)):
        errors.append('duplicate production object')
    built_sources = set(generated_sources)
    for name in objects:
        stem = name[:-2]
        sources = [stem + extension for extension in ('.c', '.S') if (ROOT / (stem + extension)).is_file()]
        if not sources and name != 'vdso/vdso_blob.o':
            errors.append(f'object has no source: {name}')
        built_sources.update(sources)
    for directory in PRODUCTION:
        for source in (ROOT / directory).rglob('*'):
            if source.suffix not in ('.c', '.S', '.h'):
                continue
            relative = source.relative_to(ROOT).as_posix()
            inputs.add(relative)
            if source.suffix in ('.c', '.S') and relative not in built_sources:
                errors.append(f'unbuilt production source: {relative}')
            if re.search(r'^\s*#\s*include\s*["<][^">]+\.(?:c|inc)[">]', source.read_text(), re.M):
                errors.append(f'implementation include: {relative}')
    for header in (ROOT / 'include/reliefnt').rglob('*.h'):
        inputs.add(header.relative_to(ROOT).as_posix())
        if re.search(r'^\s*#\s*include\s*["<][^">]+\.(?:c|inc)[">]', header.read_text(), re.M):
            errors.append(f'implementation include: {header.relative_to(ROOT)}')
    # Audit the real ignore rules, including tracked files, so a local build
    # cannot hide production sources that would be omitted from a new commit.
    # Source archives have no Git metadata and retain the structural audit.
    if (ROOT / '.git').exists():
        ignored = subprocess.run(
            ['git', '-C', str(ROOT), 'check-ignore', '--no-index', '-z', '--stdin'],
            input='\0'.join(sorted(inputs)) + '\0', text=True, capture_output=True,
        )
        if ignored.returncode not in (0, 1):
            errors.append(f'cannot audit production ignore rules: {ignored.stderr.strip()}')
        errors.extend(f'ignored production input: {name}' for name in ignored.stdout.split('\0') if name)
    manifest = json.loads((ROOT / 'scripts/kbuild-source.json').read_text())
    for name, digest in manifest['files'].items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            errors.append(f'upstream Kbuild changed: {name}')
    if errors:
        raise SystemExit('\n'.join(errors))
    print(f'PASS module layout: {len(visited)} local Kbuild lists, {len(objects)} production objects, complete source coverage and unchanged upstream engine')


if __name__ == '__main__':
    main()

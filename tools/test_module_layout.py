#!/usr/bin/env python3
"""Audit real per-directory Kbuild ownership and production source coverage."""
from pathlib import Path
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[1]
PRODUCTION = ('arch', 'block', 'drivers', 'firmware', 'fs', 'kernel', 'lib', 'mm', 'net', 'sound', 'debug', 'vdso')


def main():
    errors, objects, visited = [], [], set()

    def visit(directory):
        if directory in visited:
            errors.append(f'duplicate directory: {directory}')
            return
        visited.add(directory)
        path = ROOT / directory / 'Kbuild'
        if not path.is_file():
            errors.append(f'missing Kbuild: {directory}')
            return
        content = path.read_text().replace('\\\n', ' ')
        entries = []
        for line in content.splitlines():
            match = re.match(r'^(?:obj|always)-y\s*([:+]?=)\s*(.*?)\s*(?:#.*)?$', line)
            if not match:
                continue
            if line.startswith('obj-y') and match[1] in ('=', ':='):
                entries = []
            entries.extend(e for e in match[2].split() if e.endswith(('.o', '/')))
        for entry in entries:
            if entry.endswith('/'):
                if '/' in entry[:-1] or entry.startswith('.'):
                    errors.append(f'{path.relative_to(ROOT)}: non-local subdirectory {entry}')
                else:
                    visit(directory / entry)
            elif entry.endswith('.o'):
                if '/' in entry:
                    errors.append(f'{path.relative_to(ROOT)}: non-local object {entry}')
                objects.append((directory / entry).as_posix().removeprefix('./'))
            else:
                errors.append(f'{path.relative_to(ROOT)}: invalid object entry {entry}')

    visit(Path('.'))
    visit(Path('debug'))
    if len(objects) != len(set(objects)):
        errors.append('duplicate production object')
    built_sources = set()
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
            if source.suffix in ('.c', '.S') and relative not in built_sources:
                errors.append(f'unbuilt production source: {relative}')
            if re.search(r'^\s*#\s*include\s*["<][^">]+\.(?:c|inc)[">]', source.read_text(), re.M):
                errors.append(f'implementation include: {relative}')
    for header in (ROOT / 'include/reliefnt').rglob('*.h'):
        if re.search(r'^\s*#\s*include\s*["<][^">]+\.(?:c|inc)[">]', header.read_text(), re.M):
            errors.append(f'implementation include: {header.relative_to(ROOT)}')
    manifest = json.loads((ROOT / 'scripts/kbuild-source.json').read_text())
    for name, digest in manifest['files'].items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            errors.append(f'upstream Kbuild changed: {name}')
    if errors:
        raise SystemExit('\n'.join(errors))
    print(f'PASS module layout: {len(visited)} local Kbuild lists, {len(objects)} production objects, complete source coverage and unchanged upstream engine')


if __name__ == '__main__':
    main()

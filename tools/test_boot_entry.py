#!/usr/bin/env python3
"""Inspect the actual kernel ELF's two standard Multiboot2 entry routes."""
import argparse
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    path = args.output / 'generated/system/kernel.unstripped'
    data = path.read_bytes()
    assert data[:6] == b'\x7fELF\x02\x01'
    symbols = {}
    for line in subprocess.check_output(['nm', '-n', str(path)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    assert 'bios_start' in symbols, 'missing BIOS protected-mode entry'
    assert struct.unpack_from('<Q', data, 24)[0] == symbols['bios_start']
    magic = struct.pack('<I', 0xe85250d6)
    offset = next(i for i in range(0, min(len(data), 32768), 8) if data[i:i+4] == magic)
    fields = struct.unpack_from('<4I', data, offset)
    assert fields[1] == 0 and sum(fields) & 0xffffffff == 0
    end = offset + fields[2]
    assert end <= 32768
    cursor = offset + 16
    tags = {}
    while cursor < end:
        kind, flags, size = struct.unpack_from('<HHI', data, cursor)
        assert size >= 8 and cursor + size <= end
        tags[kind] = (flags, data[cursor+8:cursor+size])
        cursor += (size+7) & ~7
    assert cursor == end and tags[0] == (0, b'')
    for kind in (1, 7, 9):
        assert tags[kind][0] & 1, f'firmware-specific header tag {kind} is mandatory'
    assert struct.unpack('<I', tags[9][1])[0] == symbols['_start']
    first, last = symbols['__kernel_start'], symbols['__kernel_end']
    for name in ('bios_page_tables', 'bios_page_tables_end', 'bios_stack_bottom', 'bios_stack_top'):
        assert first <= symbols[name] <= last < (1 << 32), name
    assert symbols['bios_page_tables'] % 4096 == 0
    assert symbols['bios_page_tables_end'] - symbols['bios_page_tables'] == 6*4096
    assert symbols['bios_stack_top'] - symbols['bios_stack_bottom'] == 65536
    print('PASS boot ELF: BIOS ELF entry, optional EFI64 entry, valid Multiboot2 header, reserved tables and stacks')


if __name__ == '__main__':
    main()

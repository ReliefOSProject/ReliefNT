#!/usr/bin/env python3
"""Exercise the native formatter and validate its images with e2fsprogs."""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BLOCK = 4096
PER_GROUP = 32768


def run(args):
    """Run a probe, propagating any host-tool failure."""
    print("+", " ".join(map(str, args)), flush=True)
    result = subprocess.run(args, cwd=ROOT, check=False, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        print(result.stdout, end="", flush=True)
        result.check_returncode()
    return result.stdout


def inspect_journal(path, expected_groups, expected_journal):
    """Verify maps, counts, replicated superblocks and the internal JBD inode."""
    with path.open("rb") as image:
        def read_at(offset, size):
            image.seek(offset)
            data = image.read(size)
            assert len(data) == size
            return data

        def block(number):
            return read_at(number * BLOCK, BLOCK)

        def le32(data, offset=0):
            return struct.unpack_from("<I", data, offset)[0]

        def be32(data, offset=0):
            return struct.unpack_from(">I", data, offset)[0]

        sb = read_at(1024, 1024)
        total_blocks = le32(sb, 4)
        groups = (total_blocks + PER_GROUP - 1) // PER_GROUP
        assert groups == expected_groups
        assert struct.unpack_from("<H", sb, 56)[0] == 0xef53
        assert le32(sb, 92) & 4 and le32(sb, 96) == 2
        assert le32(sb, 224) == 8
        desc = read_at(BLOCK, groups * 32)
        maps = []
        free_blocks = 0
        free_inodes = 0
        for group in range(groups):
            data = desc[group * 32:(group + 1) * 32]
            bmap, imap, itable = struct.unpack_from("<III", data)
            group_blocks = min(PER_GROUP, total_blocks - group * PER_GROUP)
            bits = block(bmap)
            inode_bits = block(imap)
            declared_free, declared_inodes = struct.unpack_from("<HH", data, 12)
            actual_free = sum(not (bits[i // 8] >> (i % 8) & 1)
                              for i in range(group_blocks))
            assert declared_free == actual_free
            assert all(bits[i // 8] >> (i % 8) & 1
                       for i in range(group_blocks, PER_GROUP))
            assert declared_inodes == sum(not (inode_bits[i // 8] >> (i % 8) & 1)
                                           for i in range(8192))
            free_blocks += declared_free
            free_inodes += declared_inodes
            maps.append(bits)
            if group == 0:
                inode_table = itable
                assert inode_bits[0] & 0x80  # reserved journal inode 8
            else:
                backup = read_at(group * PER_GROUP * BLOCK, 1024)
                assert struct.unpack_from("<H", backup, 56)[0] == 0xef53
                assert struct.unpack_from("<H", backup, 90)[0] == group & 0xffff
                assert le32(backup, 92) & 4 and le32(backup, 224) == 8
                assert backup[253:336] == sb[253:336]
        assert free_blocks == le32(sb, 12) and free_inodes == le32(sb, 16)

        inode = read_at(inode_table * BLOCK + 7 * 128, 128)
        mode = struct.unpack_from("<H", inode)[0]
        size = le32(inode, 4) | le32(inode, 108) << 32
        assert mode == (0x8000 | 0o600) and size % BLOCK == 0
        assert size // BLOCK == expected_journal
        assert struct.unpack_from("<H", inode, 26)[0] == 1
        assert not (le32(inode, 32) & 0x80000)  # legacy, not extents
        pointers = struct.unpack_from("<15I", inode, 40)
        assert sb[253] == 1 and sb[268:328] == inode[40:100]
        assert le32(sb, 328) == size >> 32 and le32(sb, 332) == size & 0xffffffff
        cached = {}
        used = set()

        def claim(number):
            assert 0 < number < total_blocks and number not in used
            group, bit = divmod(number, PER_GROUP)
            assert maps[group][bit // 8] & (1 << (bit % 8))
            used.add(number)
            return number

        def indirect(number):
            if number not in cached:
                claim(number)
                cached[number] = struct.unpack("<1024I", block(number))
            return cached[number]

        journal = []
        for index in range(size // BLOCK):
            if index < 12:
                number = pointers[index]
            elif index < 12 + 1024:
                number = indirect(pointers[12])[index - 12]
            else:
                index2 = index - 12 - 1024
                leaf = indirect(pointers[13])[index2 // 1024]
                number = indirect(leaf)[index2 % 1024]
            journal.append(claim(number))
        assert le32(inode, 28) == len(used) * 8
        jsb = block(journal[0])
        assert [be32(jsb, off) for off in (0, 4, 12, 16, 20, 24, 28, 64)] == [
            0xc03b3998, 4, BLOCK, size // BLOCK, 1, 1, 0, 1]
        assert jsb[48:64] == sb[104:120]
        assert all(not any(block(number)) for number in journal[1:])
        if expected_groups > 128:
            assert len({n // PER_GROUP for n in used}) > 1
    print(f"PASS on-disk journal, bitmaps, counts and backups: {path.name}", flush=True)


def exercise_replay(work, format_binary):
    """Recover real journal transactions after each durable-write cut."""
    source = work / "ext3-replay-source.img"
    base = work / "ext3-replay-base.img"
    payload = work / "replay-payload"
    committed = work / "replay-committed.img"
    prefix = work / "replay-cut"
    journal_test = work / "journal_test"
    vfs_read = work / "vfs_read"
    print(run([format_binary, source, str(16 * 1024 * 1024 // 512), "ext3"]), end="")
    inspect_journal(source, 1, 1024)
    shutil.copyfile(source, base)
    original = b"original" * 1024
    payload.write_bytes(original)
    run(["debugfs", "-w", "-R", f"write {payload} /payload", base])
    stat = run(["debugfs", "-R", "stat /payload", base])
    match = re.search(r"Inode:\s+(\d+)", stat)
    assert match, stat
    run(["e2fsck", "-f", "-n", base])

    storage = ROOT / "block/core/storage_internal.h"
    sources = [f"fs/ext_common/{name}.c" for name in
               ("format", "checksum", "cache", "alloc", "extent", "ops", "xattr", "journal")]
    flags = ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-no-pie", "-ffunction-sections",
             "-fdata-sections", "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi"]
    cc = os.environ.get("CC", "cc")
    run([cc, *flags, "-D_GNU_SOURCE", "-DRELIEFOS_STORAGE_STANDALONE_TU",
         "-include", storage, "tools/tests/ext4_journal_test.c", *sources, "-o", journal_test])
    run([cc, *flags, "tools/tests/ext4_vfs_read_test.c", "fs/tmpfs/core.c",
         "mm/physical/proc.c", "lib/text_utf16.c", "-o", vfs_read])
    print(run([journal_test, base, match[1], "0", committed, prefix]), end="")
    run(["e2fsck", "-f", "-n", str(committed) + ".clean"])
    expected = bytes.fromhex("c03b3998") + b"c" * (BLOCK - 4) + original[BLOCK:]
    expected_file = work / "replay-expected"
    expected_file.write_bytes(expected)
    print(run([vfs_read, committed, match[1], expected_file]), end="")

    # Check every native-recovered power-cut image with an independent fsck and
    # inspect the file as an atomic old-or-new outcome.
    for cut in range(20):
        recovered = Path(f"{prefix}-{cut:02d}.img")
        run(["e2fsck", "-f", "-n", recovered])
        result = subprocess.run(["debugfs", "-R", "cat /payload", recovered], cwd=ROOT,
                                check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert result.stdout in (original, expected), f"bad payload at power cut {cut}"
    print("PASS native replay and e2fsck across 20 durable-write power cuts", flush=True)

    # Linux e2fsck also replays the committed journal made by the native writer.
    check = subprocess.run(["e2fsck", "-f", "-y", committed], cwd=ROOT,
                           text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if check.returncode not in (0, 1):
        raise RuntimeError(check.stdout)
    run(["e2fsck", "-f", "-n", committed])
    result = subprocess.run(["debugfs", "-R", "cat /payload", committed], cwd=ROOT,
                            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.stdout == expected
    print("PASS independent e2fsck replay of native committed ext3 journal", flush=True)


def suite(work):
    """Compile one bounded file backend and check small and multi-group images."""
    binary = work / "ext3_format_test"
    print(run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
               "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
               "-no-pie", "-Iinclude", "-Iinclude/uapi", "tools/tests/ext3_format_test.c",
               "fs/ext_common/mkfs.c", "-o", binary]), end="")
    small_sectors = 192 * 1024 * 1024 // 512
    large_sectors = (16 * 1024 + 128) * 1024 * 1024 // 512
    print(run([binary, work / "failures.img", str(small_sectors), "ext3-failures"]), end="")
    for kind in ("ext2", "ext3", "ext4"):
        path = work / f"{kind}.img"
        print(run([binary, path, str(small_sectors), kind]), end="")
        print(run(["e2fsck", "-f", "-n", path]), end="")
        if kind == "ext3":
            inspect_journal(path, 2, 4096)
            report = run(["dumpe2fs", "-h", path])
            assert "has_journal" in report and "Journal inode:" in report
            print(report, end="")
    # A small unusable tail group is omitted, retaining the full preceding group.
    path = work / "ext3-short-tail.img"
    print(run([binary, path, str((PER_GROUP + 100) * 8), "ext3"]), end="")
    inspect_journal(path, 1, 4096)
    print(run(["e2fsck", "-f", "-n", path]), end="")
    # JBD's smallest standard default is 1024 blocks on an 8 MiB filesystem.
    path = work / "ext3-minimum.img"
    print(run([binary, path, "16384", "ext3"]), end="")
    inspect_journal(path, 1, 1024)
    print(run(["e2fsck", "-f", "-n", path]), end="")
    exercise_replay(work, binary)
    path = work / "ext3-large-sparse.img"
    print(run([binary, path, str(large_sectors), "ext3"]), end="")
    inspect_journal(path, 129, 32768)
    print(run(["e2fsck", "-f", "-n", path]), end="")
    report = run(["dumpe2fs", "-h", path])
    assert "has_journal" in report and "Journal inode:" in report
    print(report, end="")


def main():
    """Accept an optional persistent output directory for manual inspection."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        suite(args.output_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="reliefnt-ext3-format-") as temporary:
            suite(Path(temporary))


if __name__ == "__main__":
    main()

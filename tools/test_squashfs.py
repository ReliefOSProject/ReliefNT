#!/usr/bin/env python3
"""Exercise the native driver on images made by upstream squashfs-tools."""
import os
from pathlib import Path
import random
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["fs/squashfs/io.c", "fs/squashfs/inode.c", "fs/squashfs/directory.c",
           "fs/squashfs/file.c", "fs/squashfs/inflate.c", "fs/squashfs/super.c"]


def run(*args, **kwargs):
    return subprocess.run(args, cwd=ROOT, check=True, timeout=60, **kwargs)


def make_image(tree, image, *options):
    run("mksquashfs", tree, image, "-noappend", "-quiet", "-no-progress",
        "-processors", "1", "-b", "4096", "-no-exports", "-no-xattrs",
        "-all-time", "1700000000", "-mkfs-time", "1700000000",
        "-force-uid", "1234", "-force-gid", "4321", *options)


with tempfile.TemporaryDirectory(prefix="reliefnt-squashfs-") as temporary:
    work = Path(temporary)
    tree = work / "tree"
    tree.mkdir()
    (tree / "empty").mkdir()
    (tree / "bigdir").mkdir()
    for index in range(1300):
        (tree / "bigdir" / f"file-{index:04d}").touch()
    (tree / "ldir").mkdir()
    for index in range(538):
        (tree / "ldir" / (f"{index:04d}" + "x" * 121)).touch()
    source = b"ReliefNT squashfs\n" * 900
    data = source + random.Random(1234).randbytes(4096 * 2) + b"last partial block\n"
    (tree / "data").write_bytes(data)
    (tree / "data").chmod(0o640)
    os.link(tree / "data", tree / "hardlink")
    (tree / "link").symlink_to("bigdir/../data")
    os.mkfifo(tree / "fifo")
    with (tree / "sparse").open("wb") as stream:
        stream.seek(4096 * 4)
        stream.write(b"tail")

    image = work / "fixture.squashfs"
    make_image(tree, image, "-comp", "gzip", "-no-fragments", "-Xcompression-level", "1")
    raw = work / "uncompressed.squashfs"
    make_image(tree, raw, "-no-fragments", "-noI", "-noD")
    fragments = work / "fragments.squashfs"
    make_image(tree, fragments, "-always-use-fragments")
    xz = work / "xz.squashfs"
    make_image(tree, xz, "-no-fragments", "-comp", "xz")
    empty_tree = work / "empty"
    empty_tree.mkdir()
    empty = work / "empty.squashfs"
    make_image(empty_tree, empty, "-no-fragments", "-exports")
    damaged = work / "truncated.squashfs"
    damaged.write_bytes(image.read_bytes()[:1024])
    # Check that a stock reader sees the same byte stream, not a test-only dialect.
    assert run("unsquashfs", "-cat", image, "data", stdout=subprocess.PIPE).stdout == data

    executable = work / "squashfs-test"
    run(*shlex.split(os.environ.get("HOSTCC", "cc")), "-std=c11", "-O2", "-g",
        "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
        "-Iinclude", "-Iinclude/uapi", "tools/tests/squashfs_test.c", *SOURCES,
        "-o", executable)
    run(executable, image, raw, fragments, xz, damaged, empty, tree / "data")

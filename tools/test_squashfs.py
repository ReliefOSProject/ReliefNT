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
           "fs/squashfs/file.c", "fs/squashfs/inflate.c", "fs/squashfs/super.c", "fs/squashfs/xattr.c",
           "fs/squashfs/codecs/codec.c", "fs/squashfs/codecs/lzo.c",
           "fs/squashfs/codecs/lz4/lz4.c", "fs/squashfs/codecs/lzma/LzmaDec.c",
           "fs/squashfs/codecs/zstd/zstddeclib.c"]
SOURCES += [f"fs/squashfs/codecs/xz/{name}.c" for name in (
    "xz_crc32", "xz_crc64", "xz_sha256", "xz_dec_bcj", "xz_dec_lzma2", "xz_dec_stream")]


def run(*args, **kwargs):
    return subprocess.run(args, cwd=ROOT, check=True, timeout=60, **kwargs)


def make_image(tree, image, *options):
    run("mksquashfs", tree, image, "-noappend", "-quiet", "-no-progress",
        "-processors", "1", "-b", "4096",
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
    (tree / "names").mkdir()
    (tree / "names" / ("x" * 255)).touch()
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

    for index in range(24):
        (tree / f"small-{index:02d}").write_bytes(bytes([65 + index]) * 701)
    os.setxattr(tree / "data", "user.description", b"ReliefNT")
    os.setxattr(tree / "data", "user.empty", b"")
    for path in (tree / "data", tree / "small-07"):
        os.setxattr(path, "user.shared", bytes(range(256)) * 4)

    image = work / "fixture.squashfs"
    make_image(tree, image, "-no-exports", "-no-fragments", "-no-xattrs", "-Xcompression-level", "1")
    raw = work / "uncompressed.squashfs"
    make_image(tree, raw, "-always-use-fragments", "-noI", "-noD", "-noF", "-noX")
    images = []
    for codec in ("gzip", "xz", "zstd", "lz4", "lzo"):
        fixture = work / f"{codec}.squashfs"
        make_image(tree, fixture, "-comp", codec, "-always-use-fragments")
        images.append(fixture)
    # Some distro builds of squashfs-tools omit the deprecated LZMA encoder.
    help_comp = run("mksquashfs", "-help-comp", "ALL", stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT).stdout
    if b"\n\tlzma\n" in help_comp:
        fixture = work / "lzma.squashfs"
        make_image(tree, fixture, "-comp", "lzma", "-always-use-fragments")
        images.append(fixture)
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
        "-Iinclude", "-Iinclude/uapi", "-include", "fs/squashfs/codecs/lz4/port.h",
        "tools/tests/squashfs_test.c", *SOURCES,
        "-o", executable)
    run(executable, tree / "data", damaged, empty, image, raw, *images)

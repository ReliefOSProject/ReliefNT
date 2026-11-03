#!/usr/bin/env python3
"""Run production read/write backends with real pipe queues and wait handling."""
from pathlib import Path
import argparse
from contextlib import nullcontext
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import tempfile

from kernel_test_sources import module_text

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^(?:static )?[^\n;{}]+\b" + name + r"\([^;{}]*\)\n\{", source, re.M)
    if match is None:
        raise RuntimeError("function not found: " + name)
    end = source.index("\n}\n", match.start()) + 3
    return source[match.start():end]


def io_backend(source, operation):
    name = "syscall_io_" + operation
    if re.search(r"^(?:static )?int64_t " + name + r"\(", source, re.M):
        return function(source, name)
    # Characterize the old backend without copying it into the fixture. This
    # option also permits semantic comparisons with archived pre-extraction code.
    start = source.index("    if (number == LINUX_SYS_" + operation.upper() + ") {\n")
    end = source.index("\n    }\n", start)
    body = source[source.index("\n", start) + 1:end]
    body = "\n".join(line[4:] if line.startswith("    ") else line for line in body.splitlines())
    return "static int64_t " + name + "(uint64_t a0, uint64_t a1, uint64_t a2)\n{\n" + body + "\n}\n"


def run(argv, **kwargs):
    """Retain the actual compiler and probe invocations in the suite log."""
    print("+ " + shlex.join(argv), flush=True)
    result = subprocess.run(argv, check=False, **kwargs)
    print("exit=" + str(result.returncode), flush=True)
    result.check_returncode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "kernel/syscall")
    parser.add_argument("--output", type=Path, help="New directory for extracted source and binaries")
    args = parser.parse_args()
    source = module_text("syscall_dispatch") if args.source == parser.get_default("source") else args.source.read_text()
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise RuntimeError("a C compiler is required")
    temporary = Path(os.environ.get("TMPDIR", str(Path(os.environ.get("TMPDIR", str(ROOT / "out" / "tmp")))))).resolve()
    temporary.mkdir(parents=True, exist_ok=True)
    print("SOURCE syscall.c sha256=" + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    output = args.output or (Path(os.environ["RELIEFNT_TEST_OUT"]) if os.environ.get("RELIEFNT_TEST_OUT") else None)
    if output is not None:
        output = output.resolve()
        output.mkdir(parents=True, exist_ok=False)
        workdir = nullcontext(output)
    else:
        workdir = tempfile.TemporaryDirectory(prefix="syscall-io-", dir=temporary)
    with workdir as directory:
        work = Path(directory)
        start = source.index("struct task_timerfd {")
        end = source.index("\n};", start) + 3
        bodies = [source[start:end]]
        backends = [io_backend(source, operation) for operation in ("read", "write")]
        for name in ("copy_text", "task_file_for_io", "task_pty_for_io", "task_pty_status", "file_can_read", "file_can_write", "task_device_is"):
            if name == "copy_text" and not any("copy_text(" in body for body in backends):
                continue
            if name == "task_device_is" and not any(
                "task_device_is(" in body or "scalar_prepare_user(" in body for body in backends
            ):
                continue
            bodies.append(function(source, name))
        if any("scalar_prepare_user(" in body for body in backends):
            bodies.append("#define SYSCALL_IO_HAS_SCALAR_PREPARE 1")
            for name in ("scalar_staged_file", "scalar_address_valid", "scalar_prepare_user"):
                bodies.append(function(source, name))
        bodies.extend(backends)
        (work / "syscall_io_body.inc").write_text("\n\n".join(bodies))
        for optimization in ("-O0", "-O2"):
            executable = work / ("syscall_io" + optimization)
            run([
                compiler, "-std=gnu11", optimization, "-g", "-Wall", "-Wextra",
                "-Werror", "-Wno-unused-parameter",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-I" + str(work), "-Iinclude",
                "-Iinclude", "-Iinclude", "-Iinclude/uapi",
                "tools/tests/syscall_io_test.c", "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/mount_tree.c", "fs/vfs/lookup_cache.c",
                "-o", str(executable),
            ], cwd=ROOT, env={**os.environ, "TMPDIR": str(temporary)})
            run([str(executable)], cwd=ROOT, timeout=30)
        # The same POSIX program is staged into the guest for runtime checks.
        # This run is its Linux reference, not a kernel-host-fixture result.
        executable = work / "io_runtime_linux_reference"
        run([
            compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "tools/tests/syscall_io_runtime_probe.c", "-o", str(executable),
        ], cwd=ROOT)
        print("Linux reference:", flush=True)
        run([str(executable)], cwd=ROOT,
            env={**os.environ, "TMPDIR": str(temporary)}, timeout=30)
        run([str(executable), "--null"], cwd=ROOT,
            env={**os.environ, "TMPDIR": str(temporary)}, timeout=30)


if __name__ == "__main__":
    main()

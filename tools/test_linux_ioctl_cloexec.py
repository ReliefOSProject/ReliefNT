#!/usr/bin/env python3
"""Standalone kernel FIOCLEX/FIONCLEX descriptor contracts."""
from pathlib import Path
import re
import subprocess
import tempfile
from kernel_test_sources import kernel_sources, module_text
ROOT = Path(__file__).resolve().parents[1]
LINUX_V612 = {"FIOCLEX": 0x5451, "FIONCLEX": 0x5450}
FORMAT = "native x86-64"
def read(relative):
    return (ROOT / relative).read_text()
def macro(text, name):
    match = re.search(rf"^#define {re.escape(name)}\s+(0x[0-9a-fA-F]+|\d+)", text, re.M)
    assert match, name
    return int(match.group(1), 0)
def run(command, **kwargs):
    return subprocess.run([str(item) for item in command], cwd=ROOT, check=True, **kwargs)

def test_linux_headers() -> None:
    tty = read("include/uapi/linux/tty.h")
    fcntl = read("include/uapi/linux/fcntl.h")
    assert macro(tty, "FIOCLEX") == LINUX_V612["FIOCLEX"], "FIOCLEX must match Linux v6.12"
    assert macro(tty, "FIONCLEX") == LINUX_V612["FIONCLEX"], "FIONCLEX must match Linux v6.12"
    assert macro(fcntl, "LINUX_FD_CLOEXEC") == 1
    assert macro(fcntl, "LINUX_F_GETFD") == 1
    assert macro(fcntl, "LINUX_F_SETFD") == 2
    assert macro(fcntl, "LINUX_F_DUPFD_CLOEXEC") == 1030
    assert macro(fcntl, "LINUX_O_CLOEXEC") == 0x80000
    assert macro(fcntl, "LINUX_O_PATH") == 0x200000
    for name, value in LINUX_V612.items():
        print(f"  header {name}=0x{value:04x} matches Linux v6.12 {FORMAT}")


def test_kernel_dispatch_contract() -> None:
    syscall_c = module_text("syscall_dispatch")
    sched_h = read("include/reliefnt/task/types.h") + read("include/reliefnt/sched.h")
    # The generic requests must be resolved before any category/device routing.
    dispatch = syscall_c.rindex("int64_t syscall_dispatch_regs(")
    generic = syscall_c.index("syscall_ioctl_descriptor_flags(sched_current_task(), a0")
    owned = syscall_c.index("if (syscall_fs_owns(number))", dispatch)
    assert dispatch < generic < owned, "FIOCLEX/FIONCLEX must precede device dispatch"
    path_rule = syscall_c.index("syscall_ioctl_resolve_fd(sched_current_task(), a0)")
    assert dispatch < path_rule < generic, "the O_PATH rule must run before generic ioctls"
    # Unknown requests keep the Linux generic error instead of ENOSYS.
    assert "return -RELIEFOS_ENOTTY;\n    }\n\n    return -RELIEFOS_ENOSYS;" in syscall_c
    # The signalfd branch must no longer carry a private implementation: the
    # generic handler answers FIOCLEX/FIONCLEX for it like for every other fd.
    signalfd = syscall_c.rindex("if (file && file->kind == TASK_FILE_KIND_SIGNALFD)")
    socket_dispatch = syscall_c.index("if (file && (file->flags & TASK_FILE_FLAG_SOCKET_UNIX))", signalfd)
    signalfd_block = syscall_c[signalfd:socket_dispatch]
    assert "FIOCLEX" not in signalfd_block and "FIONCLEX" not in signalfd_block
    nonblock = syscall_c.index("return syscall_ioctl_nonblock(sched_current_task()", dispatch)
    assert path_rule < nonblock < owned, "FIONBIO must use the shared generic VFS path"
    # fcntl(F_GETFD/F_SETFD) shares one storage with ioctl(FIOCLEX/FIONCLEX).
    assert "return task_fd_descriptor_flags(task, (int)a0);" in syscall_c
    assert "task_fd_set_descriptor_flags(task, (int)a0, (uint32_t)a2)" in syscall_c
    # The implicit stdio table must hold the flag, not answer success silently.
    assert "cloexec_stdio_mask" in syscall_c
    # O_PATH is FMODE_PATH, not an open flag, and must not collide with the
    # internal EPOLL bit used by the same word.
    assert macro(sched_h, "TASK_FILE_FLAG_EPOLL") == 0x00200000
    assert macro(sched_h, "TASK_FILE_FLAG_PATH") != macro(sched_h, "TASK_FILE_FLAG_EPOLL")
    assert "LINUX_O_PATH" in syscall_c and "TASK_FILE_FLAG_PATH" in syscall_c



# --------------------------------------------------------------------------- #
# Host stage 2: kernel descriptor-table unit test (real syscall.c).
# --------------------------------------------------------------------------- #

def test_kernel_descriptor_table(directory: Path) -> None:
    binary = directory / "ioctl-cloexec-table"
    sources = [
        "tools/tests/ioctl_cloexec_table_test.c",
        *kernel_sources("scheduler"),
        "kernel/sync/wait.c",
        "kernel/syscall/ipc/sysv_msg.c",
        "kernel/syscall/ipc/sysv_sem.c",
        "kernel/syscall/fs/locks.c",
        "sound/core/core.c",
        *kernel_sources("pcm"),
        "sound/core/device.c",
        "sound/core/alsa_control.c",
        "sound/core/mixer.c",
        "sound/core/oss.c",
        "sound/core/timer.c",
    ]
    run([
        "clang", "-std=c11", "-g", "-O1", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Iinclude", "-Iinclude", "-Iinclude/uapi", "-Iinclude", "-Wl,--gc-sections",
        *sources, "-o", str(binary),
    ])
    result = subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=60,
                            stdout=subprocess.PIPE, text=True)
    print("  " + result.stdout.strip())
    run([str(binary), "--invalid-fd"], timeout=60)



if __name__ == "__main__":
    test_linux_headers()
    test_kernel_dispatch_contract()
    with tempfile.TemporaryDirectory(prefix="ioctl-cloexec-") as directory:
        test_kernel_descriptor_table(Path(directory))

#!/usr/bin/env python3
"""Exercise the real swap backend with explicit failure and ownership cases."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from storage_graph_test_sources import STORAGE_GRAPH_SOURCES

ROOT = Path(__file__).resolve().parents[1]


class SwapBackendTests(unittest.TestCase):
    def test_dispatch_routes(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        with tempfile.TemporaryDirectory(prefix="swap-dispatch-", dir=os.environ["TMPDIR"]) as temp:
            work = Path(temp)
            for enabled in (True, False):
                obj, stub, binary = work / "dispatch.o", work / "stubs.c", work / "routes"
                config = ["-DCONFIG_SWAP=1"] if enabled else []
                common = [compiler, "-std=c11", "-O1", "-g", "-ffunction-sections",
                          "-fdata-sections", "-Iinclude", "-Iinclude/uapi", *config]
                subprocess.run([*common, "-c", "kernel/syscall/dispatch.c", "-o", str(obj)],
                               cwd=ROOT, check=True)
                output = subprocess.check_output(["nm", "-u", str(obj)], text=True)
                provided = {"__stack_chk_fail", "sched_current_task", "syscall_mm_swapon", "syscall_mm_swapoff",
                            "syscall_legacy_io", "task_release_syscall_file"}
                names = [line.split()[-1] for line in output.splitlines() if line.split()[-1] not in provided]
                stub.write_text("\n".join(
                    "/** @brief Trap unexercised dispatch path. @return Never. */\n"
                    "__attribute__((noreturn)) void " + name + "(void) { __builtin_trap(); }"
                    for name in names) + "\n")
                subprocess.run([*common, "-fsanitize=address,undefined", "-no-pie", "-Wl,--gc-sections",
                                "tools/tests/swap_dispatch_test.c", "kernel/syscall/memory/swap.c",
                                str(obj), str(stub), "-o", str(binary)], cwd=ROOT, check=True)
                subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=45)

    def test_activation(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        sources = ["tools/tests/swap_activation_test.c"]
        if (ROOT / "kernel/syscall/memory/swap_area.c").exists():
            sources += ["kernel/syscall/memory/swap_area.c", "kernel/syscall/memory/swap.c", "mm/swap/area.c",
                        "mm/swap/backend.c", "mm/swap/slot.c", "mm/swap/drain.c"]
        with tempfile.TemporaryDirectory(prefix="swap-activate-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "activate")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined", "-no-pie", "-ffunction-sections",
                            "-fdata-sections", "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                            *sources, "-o", binary], cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)

    def test_mapping_exclusion(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        from kernel_test_sources import kernel_sources
        with tempfile.TemporaryDirectory(prefix="swap-vma-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "vma")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1",
                            "-fsanitize=address,undefined", "-no-pie", "-ffunction-sections",
                            "-fdata-sections", "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                            "tools/tests/swap_vma_test.c", *(s for s in kernel_sources("user_memory") if s.endswith(".c")),
                            "kernel/syscall/memory/legacy_memory.c", "fs/vfs/mount_tree.c",
                            "fs/vfs/file.c", "fs/vfs/open.c", "fs/vfs/attributes.c",
                            "fs/vfs/write_access.c", "-o", binary],
                           cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)


    def test_syscall_flags(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        sources = ["tools/tests/swap_syscall_test.c"]
        if (ROOT / "kernel/syscall/memory/swap.c").exists():
            sources.append("kernel/syscall/memory/swap.c")
        with tempfile.TemporaryDirectory(prefix="swap-syscall-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "syscall")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined", "-no-pie", "-ffunction-sections",
                            "-fdata-sections", "-Wl,--gc-sections", "-Iinclude", "-Iinclude/uapi",
                            *sources, "-o", binary], cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)

    def test_disabled(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        with tempfile.TemporaryDirectory(prefix="swap-disabled-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "disabled")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-Iinclude", "-Iinclude/uapi", "tools/tests/swap_disabled_test.c",
                            "kernel/syscall/memory/swap.c", "-o", binary],
                           cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)

    def test_cache_coherence(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        with tempfile.TemporaryDirectory(prefix="swap-cache-", dir=os.environ["TMPDIR"]) as temp:
            work = Path(temp); binary = work / "coherence"; image = work / "disk.img"; data = work / "data"
            with image.open("wb") as stream:
                stream.truncate(64 * 1024 * 1024)
            data.write_bytes(bytes([0x5a]) * (3 * 4096))
            commands = [
                ["mke2fs", "-q", "-F", "-t", "ext4", "-b", "4096", "-I", "256",
                 "-O", "none,filetype,extents,64bit,flex_bg,metadata_csum,extra_isize", str(image)],
                ["debugfs", "-w", "-R", f"write {data} /payload", str(image)],
                [compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1",
                 "-fsanitize=address,undefined", "-mllvm", "-asan-globals=0", "-fno-sanitize-recover=all",
                 "-no-pie", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-Wno-address-of-packed-member", "-Iinclude", "-Iinclude/uapi",
                 "tools/tests/swap_cache_coherence_test.c", "block/core/storage_swap.c",
                 "mm/swap/area.c", "mm/swap/backend.c", "mm/swap/slot.c", "mm/cache/page_cache.c",
                 "fs/tmpfs/core.c", "fs/vfs/exec_file.c", "lib/text_utf16.c", *STORAGE_GRAPH_SOURCES,
                 "-o", str(binary)],
                [str(binary), str(image)],
            ]
            for command in commands:
                print("COMMAND", shlex.join(command), flush=True)
                subprocess.run(command, cwd=ROOT, check=True, timeout=45,
                               env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))

    def test_extents(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        with tempfile.TemporaryDirectory(prefix="swap-extents-", dir=os.environ["TMPDIR"]) as temp:
            work = Path(temp); binary = str(work / "extents"); image = work / "disk.img"; data = work / "data"
            with image.open("wb") as stream:
                stream.truncate(64 * 1024 * 1024)
            data.write_bytes(bytes([0x5a]) * 4096 + bytes(4096))
            subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4", "-b", "4096", "-I", "256",
                            "-O", "none,filetype,extents,64bit,flex_bg,metadata_csum,extra_isize", str(image)], check=True)
            subprocess.run(["debugfs", "-w", "-R", f"write {data} /payload", str(image)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1",
                            "-fsanitize=address,undefined", "-no-pie", "-ffunction-sections",
                            "-fdata-sections", "-Wl,--gc-sections", "-Wno-address-of-packed-member",
                            "-Iinclude", "-Iinclude/uapi", "tools/tests/swap_ext_map_test.c",
                            "block/core/storage_swap.c", "fs/tmpfs/core.c", "block/partition/mbr.c",
                            "lib/text_utf16.c", "fs/vfs/filesystems.c", "fs/vfs/filesystem_instance.c", "fs/vfs/write_access.c",
                            "kernel/dma/buffer.c", "drivers/storage/ahci/dma.c", "-o", binary], cwd=ROOT, check=True)
            subprocess.run([binary, str(image)], cwd=ROOT, check=True, timeout=45,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))

    def test_namespace(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        with tempfile.TemporaryDirectory(prefix="swap-namespace-", dir=os.environ["TMPDIR"]) as temp:
            work = Path(temp); binary = str(work / "namespace"); image = work / "disk.img"; data = work / "data"
            with image.open("wb") as stream:
                stream.truncate(64 * 1024 * 1024)
            data.write_bytes(bytes([0x5a]) * 8192)
            subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096", "-I", "256",
                            "-O", "none,filetype,large_file,dir_index", str(image)], check=True)
            for name in ("swap", "other"):
                subprocess.run(["debugfs", "-w", "-R", f"write {data} /{name}", str(image)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                            "-no-pie", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Wno-address-of-packed-member", "-Iinclude", "-Iinclude/uapi",
                            "tools/tests/swap_namespace_test.c", "fs/tmpfs/core.c", "lib/text_utf16.c",
                            *STORAGE_GRAPH_SOURCES,
                            "-o", binary], cwd=ROOT, check=True)
            subprocess.run([binary, str(image)], cwd=ROOT, check=True, timeout=45,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))

    def test_fixed_io(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        sources = ["tools/tests/swap_fixed_io_test.c", "block/core/storage_swap.c"]
        if (ROOT / "mm/swap/backend.c").exists():
            sources.append("mm/swap/backend.c")
        with tempfile.TemporaryDirectory(prefix="swap-fixed-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "fixed")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined", "-no-pie",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Iinclude", "-Iinclude/uapi", *sources, "-o", binary], cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)

    def test_mapping_admission(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        with tempfile.TemporaryDirectory(prefix="swap-mapping-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "mapping")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined", "-no-pie",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Iinclude", "-Iinclude/uapi", "tools/tests/swap_mapping_test.c",
                            "mm/swap/area.c", "-o", binary], cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)

    def test_raw_alias(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        with tempfile.TemporaryDirectory(prefix="swap-raw-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "raw")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-no-pie", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Wno-address-of-packed-member", "-Iinclude", "-Iinclude/uapi",
                            "tools/tests/swap_raw_alias_test.c", "block/core/storage_swap.c",
                            "fs/tmpfs/core.c", "block/partition/mbr.c", "lib/text_utf16.c",
                            "fs/vfs/filesystems.c", "fs/vfs/filesystem_instance.c", "-o", binary],
                           cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)

    def test_slots(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        sources = ["tools/tests/swap_slots_test.c"]
        for source in ("mm/swap/area.c", "mm/swap/slot.c"):
            if (ROOT / source).exists():
                sources.append(source)
        with tempfile.TemporaryDirectory(prefix="swap-slots-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "slots")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined", "-no-pie",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Iinclude", "-Iinclude/uapi", *sources, "-o", binary],
                           cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)

    def test_range_claims(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        with tempfile.TemporaryDirectory(prefix="swap-range-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "range")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined", "-no-pie",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Iinclude", "-Iinclude/uapi", "tools/tests/swap_range_test.c",
                            "block/core/storage_swap.c", "-o", binary], cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)

    def test_old_writer_mutation(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        with tempfile.TemporaryDirectory(prefix="swap-old-writer-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "mutation")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined", "-no-pie",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Iinclude", "-Iinclude/uapi", "tools/tests/swap_inode_mutation_test.c",
                            "fs/vfs/inode.c", "fs/vfs/write_access.c", "-o", binary], cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)

    def test_inode_claim(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        with tempfile.TemporaryDirectory(prefix="swap-inode-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "inode")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined", "-no-pie",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Iinclude", "-Iinclude/uapi", "tools/tests/swap_inode_test.c",
                            "fs/vfs/write_access.c", "-o", binary], cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)

    def test_transport(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        sources = ["tools/tests/swap_transport_test.c"]
        implementation = ROOT / "block/core/storage_swap.c"
        if implementation.exists():
            sources.append(str(implementation))
        with tempfile.TemporaryDirectory(prefix="swap-transport-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "transport")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined", "-no-pie",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Iinclude", "-Iinclude/uapi", *sources, "-o", binary],
                           cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)

    def test_header(self):
        compiler = shutil.which("clang")
        if not compiler:
            self.fail("clang is required")
        sources = ["tools/tests/swap_header_test.c"]
        implementation = ROOT / "mm/swap/area.c"
        if implementation.exists():
            sources.append(str(implementation))
        with tempfile.TemporaryDirectory(prefix="swap-backend-", dir=os.environ["TMPDIR"]) as temp:
            binary = str(Path(temp) / "header")
            subprocess.run([compiler, "-std=c11", "-O1", "-g", "-DCONFIG_SWAP=1", "-Wall", "-Wextra",
                            "-Werror", "-fsanitize=address,undefined", "-no-pie",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Iinclude", "-Iinclude/uapi", *sources, "-o", binary],
                           cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=45)


if __name__ == "__main__":
    unittest.main(verbosity=2)

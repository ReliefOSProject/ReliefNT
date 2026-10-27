#!/usr/bin/env python3
"""Run native module lifecycle in a private process, without host insmod/rmmod."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class ModuleLifecycle(unittest.TestCase):
    def test_real_modules_and_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-module-lifecycle-") as directory:
            work = Path(directory)
            (work / "generated").mkdir()
            (work / "generated/module_abi.h").write_text('#define RELIEFNT_MODULE_VERMAGIC "reliefnt-test-abi-1"\n')
            objects = []
            for name, source, extra in (("provider", "provider", []), ("consumer", "consumer", []),
                                        ("init_fail", "init_fail", []), ("busy", "busy", []),
                                        ("wrong_version", "provider", ['-DTEST_VERMAGIC="other-kernel"']),
                                        ("wrong_crc", "provider", ['-DTEST_IMPORT_CRC=0x99']),
                                        ("parameters", "parameters", []),
                                        ("wrong_layout", "provider", ['-DTEST_LAYOUT_CRC=0xdeadbeefu'])):
                obj = work / (name + ".o")
                subprocess.run(["clang", "--target=x86_64-elf", "-std=c11", "-O1", "-ffreestanding",
                                "-fno-pic", "-fno-pie", "-fno-common", "-mno-red-zone", "-mcmodel=large",
                                "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-DMODULE=1",
                                "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1", "-DRELIEFNT_HOST_MODULE_FIXTURE=1",
                                '-DKBUILD_MODNAME="' + name + '"', *extra, "-Iinclude", "-Iinclude/uapi",
                                "-c", "tools/tests/kmodules/" + source + ".c", "-o", str(obj)], cwd=ROOT, check=True)
                objects.append(obj)
            binary = work / "test"
            subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-pie", "-no-pie", "-DCONFIG_MODULES=1",
                            "-DCONFIG_MODULE_UNLOAD=1", "-Iinclude", "-Iinclude/uapi", "-I" + str(work),
                            "tools/tests/module_lifecycle_test.c", "tools/tests/kmodules/host_memory.c",
                            "kernel/module/elf.c", "kernel/module/layout.c", "arch/x86_64/module/relocate.c",
                            "kernel/module/lifecycle.c", "kernel/module/symbols.c", "kernel/module/params.c",
                            "kernel/module/info.c", "kernel/params/value.c", "kernel/params/arguments.c", "kernel/params/builtin.c", "kernel/params/registry.c", "-o", str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary), *map(str, objects)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

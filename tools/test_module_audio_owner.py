#!/usr/bin/env python3
"""Verify ALSA/control/mixer/PCM-VMA users against the real module unload gate."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class ModuleAudioOwner(unittest.TestCase):
    def test_open_and_mapping_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-module-audio-") as directory:
            for source in ("module_audio_owner_test.c", "module_legacy_audio_owner_test.c"):
                binary=Path(directory) / source.removesuffix(".c")
                subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
                            "-Werror", "-Wno-unused-function", "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1",
                            "-fno-pie", "-no-pie", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
                            "-Iinclude", "-Iinclude/uapi", "tools/tests/"+source,
                            "kernel/module/lifecycle.c", "kernel/params/value.c", "kernel/module/layout.c", "kernel/module/symbols.c",
                            "-o", str(binary)], cwd=ROOT, check=True)
                subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()

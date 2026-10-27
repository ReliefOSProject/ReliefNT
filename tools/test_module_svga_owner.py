#!/usr/bin/env python3
"""Run the real SVGA device model with actual callback-owner module references."""
from pathlib import Path
import subprocess
import tempfile
from test_svga import SOURCES
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-module-svga-owner-") as directory:
    generated=Path(directory)/"generated";generated.mkdir()
    (generated/"cjk_font.h").write_text("static const struct { uint32_t cp; uint8_t width; uint8_t bits[32]; } cjk_glyphs[] = {{0xfffdU, 1, {0}}};\n")
    binary=Path(directory)/"svga-native-owner"
    subprocess.run(["cc","-std=gnu11","-O1","-g","-Wall","-Wextra","-Werror",
                    "-DCONFIG_MODULES=1","-DCONFIG_MODULE_UNLOAD=1","-DRELIEFNT_SVGA_NATIVE_OWNER_TEST=1",
                    "-DSVGA_HOST_TEST","-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
                    "-fno-pie","-no-pie","-fsanitize=address,undefined","-fno-sanitize-recover=all",
                    "-I"+directory,"-Iinclude","-Iinclude/uapi","-Idrivers/video/svga",
                    "tools/tests/svga_test.c",*(str(path) for path in SOURCES),"kernel/module/lifecycle.c",
                    "kernel/module/layout.c","kernel/module/symbols.c","kernel/params/value.c","-o",str(binary)],cwd=ROOT,check=True)
    subprocess.run([str(binary)],cwd=ROOT,check=True,timeout=30)

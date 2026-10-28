#!/usr/bin/env python3
"""Run actual DRM state machines with real native-module refs and final unload."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="reliefnt-module-drm-core-owner-") as directory:
    binary = Path(directory) / "drm-native-owner"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-DCONFIG_MODULES=1", "-DCONFIG_MODULE_UNLOAD=1", "-DRELIEFNT_DRM_NATIVE_OWNER_TEST=1", '-DKBUILD_MODNAME="drm"',
                    "-include", "tools/tests/drm_owner_fixture.h", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
                    "tools/tests/drm_core_test.c", "kernel/device/mapping.c", "kernel/device/service.c",
                    "kernel/device/graphics_service.c", "drivers/video/drm/services.c", "drivers/video/drm/memory.c",
                    "kernel/module/lifecycle.c", "kernel/module/layout.c", "kernel/module/symbols.c",
                    "kernel/params/value.c", "kernel/params/arguments.c", "-o", str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30)

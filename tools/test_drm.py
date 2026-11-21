#!/usr/bin/env python3
"""Compile Linux-6.12 DRM ABI and production-core fixtures with sanitizers."""
from pathlib import Path
import subprocess
import os
import tempfile

ROOT = Path(__file__).resolve().parents[1]
INCLUDES = [
    "-Iinclude",
    "-Iinclude/uapi",
    "-Iinclude",
    "-Iinclude",
]
FIXTURES = [
    ("tools/tests/drm_abi_test.c", "drm-abi"),
    ("tests/host/svga_fifo.c", "svga-fifo"),
    ("tests/host/svga_stream.c", "svga-stream"),
    ("tests/host/svga_submit.c", "svga-submit"),
    ("tests/host/svga_dma.c", "svga-dma"),
    ("tests/host/svga_sync.c", "svga-sync"),
    ("tests/host/svga_exec.c", "svga-exec"),
    ("tests/host/svga_mksstat.c", "svga-mksstat"),
    ("tests/host/svga_screen.c", "svga-screen"),
    ("tools/tests/drm_core_test.c", "drm-core"),
    ("tools/tests/drm_vma_clear_test.c", "drm-vma-clear"),
    ("tools/tests/drm_runtime_sysfs_test.c", "drm-runtime-sysfs"),
    ("tools/tests/framebuffer_drm_test.c", "framebuffer-drm"),
    ("tools/tests/console_replay_test.c", "console-replay"),
    ("tools/tests/virtgpu_console_test.c", "virtgpu-console"),
    ("tools/tests/virtgpu_dirty_test.c", "virtgpu-dirty"),
]


def main() -> int:
    temp_parent = Path(os.environ.get("TMPDIR", str(ROOT / "out" / "tmp")))
    temp_parent.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(temp_parent)
    with tempfile.TemporaryDirectory(prefix="reliefos-drm-", dir=temp_parent) as directory:
        generated_include = Path(directory) / "include"
        generated_font = generated_include / "generated" / "cjk_font.h"
        generated_font.parent.mkdir(parents=True)
        generated_font.write_text(
            "static const struct { uint32_t cp; uint8_t width; uint8_t bits[32]; } "
            "cjk_glyphs[] = {{0xfffd, 1, {0}}};\n",
            encoding="utf-8",
        )
        for source, name in FIXTURES:
            binary = Path(directory) / name
            command = [
                "clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra",
                "-Werror", "-ffunction-sections", "-fdata-sections",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                *INCLUDES, source, "-Wl,--gc-sections", "-o", str(binary),
            ]
            if name in ("svga-fifo", "svga-submit"):
                command[1:1] = ["-Idrivers/video/drm/svga", "drivers/video/drm/svga/fifo.c"]
            if name == "svga-submit":
                command[1:1] = ["drivers/video/drm/svga/submit.c"]
            if name == "svga-dma":
                command[1:1] = [f"drivers/video/drm/svga/{part}.c" for part in ("dma", "surface", "context", "binding")]
            if name == "svga-sync":
                command[1:1] = [f"drivers/video/drm/svga/{part}.c"
                                for part in ("fence", "query", "buffer")]
            if name == "svga-exec":
                command[1:1] = [f"drivers/video/drm/svga/{part}.c"
                                for part in ("exec", "buffer")]
            if name == "svga-mksstat":
                command[1:1] = ["drivers/video/drm/svga/mksstat.c"]
            if name == "svga-screen":
                command[1:1] = ["drivers/video/drm/svga/screen.c"]
            if name == "drm-core":
                command[1:1] = ['-DKBUILD_MODNAME="drm"', "kernel/params/value.c", "kernel/params/arguments.c",
                                "kernel/device/service.c", "kernel/device/graphics_service.c",
                                "drivers/video/drm/services.c", "drivers/video/drm/memory.c",
                                "drivers/video/drm/sync.c"]
            if name in ("drm-core", "drm-vma-clear"):
                command[1:1] = ["kernel/device/mapping.c"]
            if name == "drm-vma-clear":
                command[1:1] = ["drivers/video/drm/mapping.c"]
            if name in ("framebuffer-drm", "console-replay"):
                command[1:1] = [f"-I{generated_include}",
                                "-Idrivers/video/framebuffer"]
            print("$ " + " ".join(command))
            subprocess.run(command, cwd=ROOT, check=True)
            if name == "console-replay":
                shadow_output = Path(directory) / "console-replay-shadow.bin"
                fallback_output = Path(directory) / "console-replay-fallback.bin"
                runs = [
                    ([str(binary), str(shadow_output), "shadow"], "shadow"),
                    ([str(binary), str(fallback_output), "fallback"], "fallback"),
                ]
                for arguments, mode in runs:
                    result = subprocess.run(arguments, cwd=ROOT, capture_output=True,
                                            text=True, timeout=60)
                    print(f"[{mode}] {result.stdout}", end="")
                    if result.returncode:
                        print(result.stderr, end="")
                        return result.returncode
                if shadow_output.read_bytes() != fallback_output.read_bytes():
                    print("console-replay: shadow and allocation-failure fallback differ")
                    return 1
                reference_output = ROOT / "out/task6c-console-reference/reference.bin"
                if reference_output.is_file():
                    reference = reference_output.read_bytes()
                    if shadow_output.read_bytes() != reference:
                        print("console-replay: shadow output differs from frozen direct-render reference")
                        return 1
                    if fallback_output.read_bytes() != reference:
                        print("console-replay: fallback output differs from frozen direct-render reference")
                        return 1
                    print("console-replay: both paths match frozen direct-render reference")
                print("console-replay: shadow and fallback outputs match")
            else:
                result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True,
                                        text=True, timeout=30)
                print(result.stdout, end="")
                if result.returncode:
                    print(result.stderr, end="")
                    return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

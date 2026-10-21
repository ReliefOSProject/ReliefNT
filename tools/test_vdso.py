#!/usr/bin/env python3
"""Build and exercise ReliefNT's x86-64 vDSO ELF and callable ABI."""

from pathlib import Path
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile


KERNEL_ROOT = Path(__file__).resolve().parents[1]


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=False, text=True, **kwargs)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def tool(*names: str) -> str:
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    raise AssertionError(f"required inspection tool not found: {' or '.join(names)}")


def output(command: list[str]) -> str:
    result = run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    require(result.returncode == 0,
            f"command failed ({result.returncode}): {' '.join(command)}\n{result.stdout}")
    return result.stdout


def check_elf(vdso: Path, vdso_blob: Path) -> None:
    readelf = tool("readelf", "llvm-readelf")
    header = output([readelf, "-hW", str(vdso)])
    require(re.search(r"Type:\s+DYN\b", header) is not None, "vDSO is not ET_DYN")
    require("X86-64" in header, "vDSO is not ELF64 x86-64")

    phdrs = output([readelf, "-lW", str(vdso)])
    require(re.search(r"^\s*LOAD\s", phdrs, re.M) is not None, "vDSO has no PT_LOAD")
    require(re.search(r"^\s*DYNAMIC\s", phdrs, re.M) is not None, "vDSO has no PT_DYNAMIC")
    require(re.search(r"^\s*INTERP\s", phdrs, re.M) is None,
            "freestanding vDSO unexpectedly has a PT_INTERP")
    load_lines = [line for line in phdrs.splitlines()
                  if line.split() and line.split()[0] == "LOAD"]
    require(len(load_lines) == 1,
            f"vDSO must use one whole-image PT_LOAD, found {len(load_lines)}")
    load_flags = load_lines[0].split()[6:-1]
    require("R" in load_flags and "E" in load_flags and "W" not in load_flags,
            f"vDSO PT_LOAD must be read-only and executable: {load_lines[0]}")
    dynamic_lines = [line for line in phdrs.splitlines()
                     if line.split() and line.split()[0] == "DYNAMIC"]
    require(dynamic_lines and dynamic_lines[0].split()[6:-1] == ["R"],
            f"PT_DYNAMIC must be read-only: {dynamic_lines}")
    stack_lines = [line for line in phdrs.splitlines()
                   if line.split() and line.split()[0] == "GNU_STACK"]
    require(stack_lines, "vDSO has no explicit non-executable GNU_STACK")
    require("E" not in stack_lines[0].split()[6:-1],
            f"vDSO requests an executable stack: {stack_lines[0]}")
    dynamic = output([readelf, "-dW", str(vdso)])
    require("[linux-vdso.so.1]" in dynamic, "vDSO SONAME is not linux-vdso.so.1")
    require("(NEEDED)" not in dynamic, "vDSO has a DT_NEEDED dependency")
    for tag in ("(HASH)", "(GNU_HASH)", "(STRTAB)", "(SYMTAB)", "(VERSYM)", "(VERDEF)"):
        require(tag in dynamic, f"vDSO dynamic table is missing {tag}")
    require("(TEXTREL)" not in dynamic, "vDSO requests text relocations")
    require("(PLTGOT)" not in dynamic and "(JMPREL)" not in dynamic,
            "vDSO has a PLT/GOT dynamic dependency")

    relocations = output([readelf, "-rW", str(vdso)])
    require(re.search(r"There are no relocations", relocations, re.I) is not None,
            f"vDSO has runtime relocations:\n{relocations}")

    dynsym = output([readelf, "--dyn-syms", "-W", str(vdso)])
    for name in ("__vdso_clock_gettime", "clock_gettime",
                 "__vdso_clock_getres", "clock_getres"):
        require(re.search(rf"\b{name}@@LINUX_2\.6\b", dynsym) is not None,
                f"dynamic symbol {name}@LINUX_2.6 is missing")
    require("__vvar_data" not in dynsym, "hidden __vvar_data leaked into .dynsym")
    undefined = [line for line in dynsym.splitlines()
                 if re.search(r"\bUND\s+\S", line)]
    require(not undefined, f"vDSO has undefined dynamic symbols: {undefined}")
    versions = output([readelf, "-VW", str(vdso)])
    require("LINUX_2.6" in versions, "vDSO has no LINUX_2.6 version definition")

    symtab = output([readelf, "-sW", str(vdso)])
    vvar = re.search(
        r"^\s*\d+:\s+([0-9a-fA-F]+)\s+\d+\s+\S+\s+\S+\s+HIDDEN\s+ABS\s+__vvar_data\s*$",
        symtab, re.M)
    require(vvar is not None, "hidden absolute __vvar_data symbol is missing")
    require(int(vvar.group(1), 16) == (1 << 64) - 4096,
            "__vvar_data is not ELF base minus one page")

    sections = output([readelf, "-SW", str(vdso)])
    require(not re.search(r"\s\.got(?:\.\S+)?\s", sections),
            "vDSO unexpectedly contains a GOT section")

    nm = tool("nm", "llvm-nm")
    blob_symbols = output([nm, "-g", "--defined-only", str(vdso_blob)])
    require("reliefnt_vdso_image_start" in blob_symbols,
            "kernel blob is missing reliefnt_vdso_image_start")
    require("reliefnt_vdso_image_end" in blob_symbols,
            "kernel blob is missing reliefnt_vdso_image_end")
    objcopy = tool("objcopy", "llvm-objcopy")
    with tempfile.TemporaryDirectory(prefix="vdso-embed-", dir=vdso.parent) as temp_dir:
        embedded = Path(temp_dir) / "embedded.bin"
        result = run([objcopy, "--dump-section",
                      f".rodata.reliefnt_vdso={embedded}", str(vdso_blob)],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        require(result.returncode == 0,
                f"cannot extract embedded vDSO bytes:\n{result.stdout}")
        require(embedded.read_bytes() == vdso.read_bytes(),
                "embedded vDSO bytes differ from generated ELF")


def check_runtime(out: Path, vdso: Path) -> None:
    binary = out / "tools" / "tests" / "vdso_runtime_test"
    require(binary.is_file(), f"Kbuild did not produce runtime test {binary}")
    try:
        result = run([str(binary), str(vdso)], stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, timeout=10)
    except subprocess.TimeoutExpired as error:
        raise AssertionError("runtime vDSO test hung, including bounded seq retry") from error
    require(result.returncode == 0,
            f"runtime vDSO test failed ({result.returncode}):\n{result.stdout}")
    print(result.stdout, end="")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=KERNEL_ROOT / "out" / "vdso-task1-test",
                        help="isolated kernel build output directory")
    parser.add_argument("--no-build", action="store_true", help="inspect prerequisites already built by Make")
    args = parser.parse_args()
    out = args.output.resolve()
    vdso = out / "vdso" / "vdso.so"
    vdso_blob = out / "vdso" / "vdso_blob.o"

    env = os.environ.copy()
    tmpdir = env.get("TMPDIR") or str(KERNEL_ROOT / "out" / "tmp")
    Path(tmpdir).mkdir(parents=True, exist_ok=True)
    env["TMPDIR"] = tmpdir

    command = ["make", "-C", str(KERNEL_ROOT), f"O={out}", "vdso", "vdso-runtime-test"]
    print("+", " ".join(command), flush=True) if not args.no_build else None
    result = subprocess.run(command, env=env, check=False) if not args.no_build else subprocess.CompletedProcess(command, 0)
    try:
        require(result.returncode == 0,
                f"`make vdso` exited {result.returncode}")
        require(vdso.is_file(), f"build did not produce {vdso}")
        require(vdso_blob.is_file(), f"build did not produce {vdso_blob}")
        check_elf(vdso, vdso_blob)
        check_runtime(out, vdso)
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print("PASS: ELF metadata, symbol versions, adjacent vvar, and function behavior")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

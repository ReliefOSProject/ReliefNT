#!/usr/bin/env python3
"""Freeze UAPI wire layouts and constant values; fail on any ABI drift.

The golden file records, for every struct/union defined in the exported
UAPI surface: sizeof, align and every member offset, plus the numeric
value of the load-bearing constants (ioctl numbers, magics, versions).
Run with --update after an INTENTIONAL ABI change; an accidental change
must fail this test.

Layout data comes from clang's record-layout dump of a probe TU that
instantiates every discovered wire struct, so new structs are picked up
automatically (and require a golden update to pass).
"""
from pathlib import Path
import argparse
import json
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GOLDEN = ROOT / "tools/tests/abi_layout_golden.json"

# Headers that hold user/kernel wire contracts (pre- and post-split).
WIRE_HEADERS = [
    "uapi/reliefos/auth_user.h", "uapi/reliefos/fs_abi.h", "uapi/reliefos/net_control.h",
    "uapi/reliefos/rootfs.h", "uapi/reliefos/syscall_abi.h",
    "reliefos/audio.h", "reliefos/auth.h", "reliefos/device.h",
    "reliefos/driver.h", "reliefos/elf_abi.h", "reliefos/gpu.h", "reliefos/inputm.h",
    "reliefos/kernel_debug.h", "reliefos/net.h", "reliefos/pty.h", "reliefos/signal.h",
    "reliefos/startup.h", "reliefos/system.h",
    # split targets, once they exist
    "uapi/reliefos/audio_abi.h", "uapi/reliefos/auth_abi.h", "uapi/reliefos/device_abi.h",
    "uapi/reliefos/driver_abi.h", "uapi/reliefos/gpu_abi.h", "uapi/reliefos/inputm_abi.h",
    "uapi/reliefos/kernel_debug_abi.h", "uapi/reliefos/net_abi.h",
    "uapi/reliefos/startup_abi.h", "uapi/reliefos/system_abi.h",
    "uapi/reliefos/pty_abi.h", "uapi/reliefos/signal_abi.h",
]

# Numeric constants whose value is ABI (ioctl/magic/version/enum encodings).
CONSTANTS = [
    "RELIEFOS_SYS_NICE",
    "RELIEFOS_DRIVER_MODULE_MAGIC", "RELIEFOS_DRIVER_ABI_VERSION",
    "RELIEFOS_DRIVER_CONTROL_IOCTL",
    "RELIEFOS_VT_GETGENERATION", "RELIEFOS_EVIOCSVT",
    "RELIEFOS_NET_CONTROL_IOCTL",
    "RELIEFOS_KERNEL_DEBUG_CONTROL_GET_STATE", "RELIEFOS_KERNEL_DEBUG_CONTROL_SET_ENABLED",
    "RELIEFOS_KERNEL_DEBUG_CONTROL_ARM_NEXT_BOOT", "RELIEFOS_KERNEL_DEBUG_CONTROL_CLEAR",
    "RELIEFOS_IOCTL_GPU_INFO", "RELIEFOS_IOCTL_GPU_CREATE", "RELIEFOS_IOCTL_GPU_DESTROY",
    "RELIEFOS_IOCTL_GPU_RENDER", "RELIEFOS_IOCTL_GPU_DIAGNOSTICS",
    "RELIEFOS_AUTH_ROLE_NONE", "RELIEFOS_AUTH_ROLE_USER", "RELIEFOS_AUTH_ROLE_ADMIN",
    "RELIEFOS_AUTH_USER_DISABLED", "RELIEFOS_AUTH_UPDATE_ROLE", "RELIEFOS_AUTH_UPDATE_FLAGS",
    "RELIEFOS_PERF_MAX_CPUS",
    "RELIEFOS_TASK_AFFINITY_GET", "RELIEFOS_TASK_AFFINITY_SET",
    "RELIEFOS_PTY_NCCS", "RELIEFOS_PTY_PATH_LEN",
    "RELIEFOS_INPUTM_MAX_PROVIDERS", "RELIEFOS_INPUTM_MAX_CANDIDATES",
    "RELIEFOS_FS_TYPE_FILE", "RELIEFOS_FS_TYPE_DIR",
]

INCLUDES = ["-I", str(ROOT / "include/uapi"), "-I", str(ROOT / "include"),
            "-I", str(ROOT / "userland/libc/include"), "-I", str(ROOT / "include/uapi")]


def wire_header_paths():
    """Existing wire headers; optional split targets are skipped until present."""
    paths = []
    for relative in WIRE_HEADERS:
        path = ROOT / "include" / relative
        if path.is_file():
            paths.append(path)
    return paths


def discover_structs(paths):
    """Every struct/union *definition* in the wire headers (skip forward decls).

    Maps name -> "struct"/"union" kind so the probe uses the right keyword.
    """
    found = {}
    definition = re.compile(r"^\s*struct\s+(reliefos_\w+)\s*\{", re.M)
    union_definition = re.compile(r"^\s*union\s+(reliefos_\w+)\s*\{", re.M)
    for path in paths:
        text = path.read_text()
        for match in definition.finditer(text):
            found.setdefault(match.group(1), "struct")
        for match in union_definition.finditer(text):
            found.setdefault(match.group(1), "union")
    return found


def probe_source(structs):
    lines = ['#include <reliefos/%s>' % p.name for p in
             sorted({p for p in wire_header_paths()})]
    # Include by wire path so uapi/reliefos files resolve as <reliefos/...> too.
    # A sizeof assertion forces clang to complete each record's layout here;
    # -fdump-record-layouts only reports records whose layout was computed.
    for name in sorted(structs):
        kind = structs[name]
        lines.append('_Static_assert(sizeof(%s %s) > 0, "layout %s");'
                     % (kind, name, name))
    lines.append("#include <stdio.h>")
    lines.append("int main(void) {")
    for const in CONSTANTS:
        lines.append('  printf("%%s=%%lld\\n", "%s", (long long)(%s));' % (const, const))
    lines.append("  return 0; }")
    return "\n".join(lines) + "\n"


# clang names anonymous members after the absolute path of the header they live
# in ("(unnamed at /abs/path/include/uapi/reliefos/net_control.h:18:5)"). That is
# build-location trivia, not ABI: collapse the directory prefix to the
# repository-relative include path so the golden file is location independent.
_DECL_PATH = re.compile(r"/[^() ]*/include/")


def normalize_decl(decl):
    return _DECL_PATH.sub("include/", decl)


def parse_record_layouts(dump, wanted):
    """clang -fdump-record-layouts text -> {record: {size, align, members}}."""
    records = {}
    current = None
    start = re.compile(r"^\s*\d+ \| (?:struct|union) (reliefos_\w+)\s*$")
    member = re.compile(r"^\s*(\d+) \| (.+)$")
    finish = re.compile(r"^\s*\| \[sizeof=(\d+), align=(\d+)\]")
    for line in dump.splitlines():
        match = start.match(line)
        if match:
            current = match.group(1)
            if current in wanted:
                records[current] = {"members": []}
            else:
                current = None
            continue
        if current is None:
            continue
        match = finish.match(line)
        if match:
            records[current]["size"] = int(match.group(1))
            records[current]["align"] = int(match.group(2))
            current = None
            continue
        match = member.match(line)
        if match:
            records[current]["members"].append([int(match.group(1)),
                                                normalize_decl(match.group(2).strip())])
    return records


def collect(structs):
    with tempfile.TemporaryDirectory(prefix="reliefos-abi-") as directory:
        temp = Path(directory)
        source = temp / "probe.c"
        source.write_text(probe_source(structs))
        consts = {}
        if CONSTANTS:
            binary = temp / "probe"
            subprocess.run(["clang", *INCLUDES, str(source), "-o", str(binary)],
                           check=True, capture_output=True)
            output = subprocess.run([str(binary)], check=True, capture_output=True,
                                    text=True).stdout
            for line in output.splitlines():
                key, _, value = line.partition("=")
                consts[key] = int(value)
        dump = subprocess.run(
            ["clang", "-fsyntax-only", "-Xclang", "-fdump-record-layouts",
             *INCLUDES, str(source)],
            check=True, capture_output=True, text=True).stdout
        layout = parse_record_layouts(dump, set(structs))
        missing = sorted(set(structs) - set(layout))
        if missing:
            raise SystemExit(f"layout dump missed structs: {missing}")
        return {"consts": consts, "layout": layout}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update", action="store_true",
                        help="rewrite the golden file after an intentional change")
    args = parser.parse_args()
    paths = wire_header_paths()
    structs = discover_structs(paths)
    current = collect(structs)
    if args.update:
        GOLDEN.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN.write_text(json.dumps(current, indent=1, sort_keys=True) + "\n")
        print(f"updated {GOLDEN.relative_to(ROOT)}: "
              f"{len(current['layout'])} records, {len(current['consts'])} constants")
        return
    if not GOLDEN.is_file():
        raise SystemExit(f"missing golden {GOLDEN}; run with --update first")
    golden = json.loads(GOLDEN.read_text())
    # The golden was recorded under a different absolute source path; the
    # normalization makes both sides comparable regardless of checkout location.
    for record in golden.get("layout", {}).values():
        record["members"] = [[offset, normalize_decl(decl)]
                             for offset, decl in record["members"]]
    failures = []
    for record in sorted(set(golden["layout"]) | set(current["layout"])):
        want, have = golden["layout"].get(record), current["layout"].get(record)
        if want is None:
            failures.append(f"{record}: new record (run --update after review)")
        elif have is None:
            failures.append(f"{record}: disappeared")
        elif want != have:
            for key in ("size", "align"):
                if want.get(key) != have.get(key):
                    failures.append(f"{record}: {key} {want.get(key)} -> {have.get(key)}")
            want_members = {tuple(m) for m in want["members"]}
            have_members = {tuple(m) for m in have["members"]}
            for offset, decl in sorted(want_members - have_members):
                failures.append(f"{record}: member lost/moved: {offset} {decl}")
            for offset, decl in sorted(have_members - want_members):
                failures.append(f"{record}: member added/moved: {offset} {decl}")
    for const in sorted(set(golden["consts"]) | set(current["consts"])):
        want, have = golden["consts"].get(const), current["consts"].get(const)
        if want != have:
            failures.append(f"constant {const}: {want} -> {have}")
    if failures:
        print("ABI DRIFT detected (this is what the test is for):")
        for line in failures:
            print("  " + line)
        raise SystemExit(1)
    print(f"PASS ABI layout: {len(current['layout'])} records, "
          f"{len(current['consts'])} constants unchanged")


if __name__ == "__main__":
    main()

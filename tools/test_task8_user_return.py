#!/usr/bin/env python3
"""Exercise extracted production user-return gates with a staged exec and swap."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from test_user_return_gate import BODIES, ROOT, host_function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="New directory for retained source, binary and logs")
    args = parser.parse_args()
    output = args.output or (Path(os.environ["RELIEFNT_TEST_OUT"]) if os.environ.get("RELIEFNT_TEST_OUT") else None)
    if output is None:
        output = Path(tempfile.mkdtemp(prefix="task8-user-return-", dir=os.environ.get("TMPDIR")))
    else:
        output = output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    for name, source, symbol in BODIES:
        body = host_function(ROOT / source, symbol)
        (output / name).write_text(body)
    sources = ["tools/tests/user_return_gate_test.c", "kernel/syscall/fs/lookup_state.c",
               "tools/tests/task8_host_stubs/no_prlimit_identity.c",
               "kernel/syscall/io/vector.c", "fs/vfs/mount_tree.c", "fs/vfs/lookup_cache.c", "fs/vfs/path.c",
               "drivers/usb/core/registry.c", "drivers/usb/core/work.c", "kernel/device/driver_manager_phase.c",
               "block/device/registry.c"]
    binary = output / "user-return-swap"
    compile_command = ["clang", "-std=c11", "-pthread", "-O1", "-g", "-DCONFIG_SWAP=1",
                       "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                       "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-ffunction-sections",
                       "-fdata-sections", "-Wl,--gc-sections", "-I" + str(output), "-Iinclude", "-Iinclude/uapi",
                       *sources, "-o", str(binary)]
    commands = []
    for index, argv in enumerate((compile_command, [str(binary)])):
        print("+ " + shlex.join(argv), flush=True)
        log = output / f"command-{index}.log"
        try:
            with log.open("xb") as stream:
                result = subprocess.run(argv, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT,
                                        timeout=90, check=False)
            status = result.returncode
        except subprocess.TimeoutExpired:
            status = "timeout"
        except OSError as error:
            status = "exec-error"
            with log.open("ab") as stream:
                stream.write((str(error) + "\n").encode())
        print(log.read_text(errors="replace"), end="", flush=True)
        commands.append({"argv": argv, "cwd": str(ROOT), "returncode": status, "log": log.name})
        if status != 0:
            break
    source_paths = set(sources + [source for _, source, _ in BODIES] +
                       ["tools/test_task8_user_return.py", "tools/test_user_return_gate.py"])
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    record = {"environment": {key: os.environ[key] for key in
              ("TMPDIR", "PYTHONDONTWRITEBYTECODE", "RELIEFNT_TEST_OUT") if key in os.environ},
              "commands": commands,
              "sources_sha256": {source: sha(ROOT / source) for source in sorted(source_paths)},
              "artifacts_sha256": {path.name: sha(path) for path in sorted(output.iterdir()) if path.is_file()}}
    (output / "results.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"Retained user-return evidence: {output}", flush=True)
    return int(len(commands) != 2 or commands[-1]["returncode"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())

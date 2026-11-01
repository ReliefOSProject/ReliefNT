#!/usr/bin/env python3
"""Run the explicitly listed Task8 host boundary and ownership fixtures.

The manifest lists every compiler command and source input; no repository
scanning, external handoff files, generated preimages, or guest are required.
Stub sources deliberately trap if a fixture reaches an unmodeled dependency.
Use --fixture NAME to run a focused subset, or --list to inspect the inventory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tools/tests/task8_host_fixtures.json"


def source_closure(argv, output, fixture, index, hashes):
    """Record the actual local include closure for each C compilation input.

    Keep macro and include-search options from the explicit compiler command.
    The generated dependency log is retained alongside each run; a failed scan
    fails the fixture rather than silently claiming incomplete provenance.
    """
    if not argv or Path(argv[0]).name not in ("cc", "clang", "gcc"):
        return []
    options = []
    skip = False
    takes_value = False
    for arg in argv[1:]:
        if skip:
            skip = False
        elif takes_value:
            options.append(arg)
            takes_value = False
        elif arg == "-o":
            skip = True
        elif arg in ("-I", "-D", "-U", "-isystem", "-include"):
            options.append(arg)
            takes_value = True
        elif arg.startswith("-") and arg not in ("-c", "-no-pie") and not arg.startswith("-Wl,"):
            options.append(arg)
    scans = []
    for number, source in enumerate(a for a in argv[1:] if a.endswith((".c", ".h"))):
        path = Path(source)
        if not path.is_absolute():
            path = ROOT / path
        if not path.is_file():
            continue
        quiet = ["-Wno-unused-command-line-argument"] if Path(argv[0]).name == "clang" else []
        command = [argv[0], *options, *quiet, "-MM", "-MT", "source", source]
        result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, check=False)
        log = output / f"{fixture}.{index}.deps-{number}.log"
        log.write_text(result.stdout)
        scans.append({"argv": command, "returncode": result.returncode, "log": log.name})
        if result.returncode:
            continue
        for item in shlex.split(result.stdout.replace("\\\n", " "))[1:]:
            dependency = Path(item)
            if not dependency.is_absolute():
                dependency = ROOT / dependency
            dependency = dependency.resolve()
            if dependency.is_file() and dependency.is_relative_to(ROOT):
                hashes[str(dependency.relative_to(ROOT))] = hashlib.sha256(dependency.read_bytes()).hexdigest()
    return scans


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", action="append", default=[], help="Run only this named fixture; repeatable")
    parser.add_argument("--list", action="store_true", help="Print the fixed fixture names")
    parser.add_argument("--output", type=Path, help="New directory for retained logs, objects, binaries and hashes")
    args = parser.parse_args()
    fixtures = json.loads(MANIFEST.read_text())
    names = [fixture["name"] for fixture in fixtures]
    if len(names) != len(set(names)):
        raise SystemExit("duplicate Task8 fixture name")
    if args.list:
        print("\n".join(names))
        return 0
    missing = set(args.fixture) - set(names)
    if missing:
        raise SystemExit("unknown fixture(s): " + ", ".join(sorted(missing)))
    selected = [fixture for fixture in fixtures if not args.fixture or fixture["name"] in args.fixture]
    failures = []
    output = args.output or (Path(os.environ["RELIEFNT_TEST_OUT"]) if os.environ.get("RELIEFNT_TEST_OUT") else None)
    if output is not None:
        output = output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="task8-frontdoors-", dir=os.environ.get("TMPDIR")))
    commands_run = []
    source_hashes = {}
    for source in (MANIFEST, Path(__file__).resolve()):
        source_hashes[str(source.relative_to(ROOT))] = hashlib.sha256(source.read_bytes()).hexdigest()
    for fixture in selected:
        print(f"\n=== {fixture['name']} ===", flush=True)
        for index, command in enumerate(fixture["commands"] + [fixture["run"]]):
            argv = [arg.replace("{tmp}", str(output)) for arg in command]
            print("+ " + shlex.join(argv), flush=True)
            for argument in command:
                source = ROOT / argument
                if not argument.startswith(("-", "{tmp}")) and source.is_file():
                    source_hashes[argument] = hashlib.sha256(source.read_bytes()).hexdigest()
            scans = source_closure(argv, output, fixture["name"], index, source_hashes)
            log = output / f"{fixture['name']}.{index}.log"
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
            commands_run.append({"fixture": fixture["name"], "argv": argv, "cwd": str(ROOT),
                                 "returncode": status, "log": log.name, "dependency_scans": scans})
            failed_scan = next((scan["returncode"] for scan in scans if scan["returncode"]), 0)
            if status != 0 or failed_scan:
                failure = status if status != 0 else f"dependency-scan={failed_scan}"
                failures.append((fixture["name"], failure))
                print(f"FAIL {fixture['name']}: {failure}", flush=True)
                break
        else:
            print(f"PASS {fixture['name']}", flush=True)
    artifacts = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in sorted(output.iterdir()) if path.is_file()}
    result = {"environment": {key: os.environ[key] for key in
              ("TMPDIR", "PYTHONDONTWRITEBYTECODE", "RELIEFNT_TEST_OUT") if key in os.environ},
              "commands": commands_run, "sources_sha256": source_hashes,
              "artifacts_sha256": artifacts, "failures": failures}
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"Retained Task8 evidence: {output}", flush=True)
    if failures:
        print("Task8 failures: " + ", ".join(f"{name}={status}" for name, status in failures), flush=True)
        return 1
    print(f"Task8: {len(selected)} fixtures passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

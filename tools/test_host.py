#!/usr/bin/env python3
"""Run the explicit standalone kernel host suites and keep every failure report."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tools/tests/host-suites.list"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", action="append", default=[], help="suite name from host-suites.list; repeatable")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--output", type=Path, default=ROOT / "out/host-tests")
    args = parser.parse_args()
    suites = {}
    for line in MANIFEST.read_text().splitlines():
        words = shlex.split(line, comments=True)
        if not words:
            continue
        name, *command = words
        if name in suites or not command:
            parser.error(f"invalid or duplicate suite: {line}")
        source = (ROOT / command[0]).resolve()
        if not source.is_relative_to(ROOT) or not source.is_file():
            parser.error(f"suite source is outside this kernel or missing: {line}")
        suites[name] = command
    unknown = set(args.suite) - suites.keys()
    if unknown:
        parser.error("unknown suites: " + ", ".join(sorted(unknown)))
    selected = args.suite or list(suites)
    if args.list:
        print("\n".join(selected))
        return 0
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    temporary = Path(os.environ.get("TMPDIR", str(output / "tmp"))).resolve()
    temporary.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, "TMPDIR": str(temporary), "PYTHONDONTWRITEBYTECODE": "1"}
    results = []
    for name in selected:
        start = time.monotonic()
        log = output / (name + ".log")
        with log.open("w") as stream:
            result = subprocess.run([sys.executable, *[word.replace("{output}", str(output)) for word in suites[name]]], cwd=ROOT,
                                    env={**env, "RELIEFNT_TEST_OUT": str(output / name)},
                                    stdout=stream, stderr=subprocess.STDOUT)
        record = {"suite": name, "exit": result.returncode,
                  "seconds": round(time.monotonic() - start, 2), "log": str(log)}
        results.append(record)
        print(f"{'PASS' if result.returncode == 0 else 'FAIL'} {name} ({record['seconds']}s): {log}", flush=True)
        if result.returncode:
            print("\n".join(log.read_text(errors="replace").splitlines()[-14:]), flush=True)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    failed = sum(item["exit"] != 0 for item in results)
    print(f"kernel host suites: {len(results) - failed} passed, {failed} failed", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    sys.exit(main())

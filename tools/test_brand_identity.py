#!/usr/bin/env python3
"""Require a justified entry for legacy identities in this kernel repository."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
records = {}
for line in (ROOT / "tools/tests/brand-allowlist.tsv").read_text().splitlines():
    if not line or line.startswith("#"):
        continue
    category, path, reason = line.split("\t")
    assert category in {"compatibility", "history", "external-url", "attribution"}, line
    assert path not in records and reason, line
    records[path] = reason
if (ROOT / ".git").exists():
    paths = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT).decode().split("\0")
else:
    # Source archives have no Git metadata. Walk sources only; outputs/caches
    # are never part of a branding audit.
    paths = []
    import os
    for directory, folders, files in os.walk(ROOT):
        folders[:] = [name for name in folders if name not in {".git", "out", "build", "cache", "__pycache__"}]
        paths.extend(str((Path(directory) / name).relative_to(ROOT)) for name in files)

missing = []
for name in sorted(set(paths)):
    path = ROOT / name
    if not name or not path.is_file():
        continue
    try:
        text = path.read_text()
    except UnicodeError:
        continue
    if re.search(r"leonos|ntclks", name + "\n" + text, re.I) and name not in records:
        missing.append(name)
assert not missing, "Unreviewed legacy identities: " + ", ".join(missing)
print("PASS standalone kernel identity audit")

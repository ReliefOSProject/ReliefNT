#!/usr/bin/env python3
"""Assert that kernel host tests are self-contained and explicitly registered."""
import ast
from pathlib import Path
import re
import shlex
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TestOwnership(unittest.TestCase):
    def test_no_parent_checkout_or_os_source_dependency(self):
        failures = []
        for directory in (ROOT / "tools", ROOT / "tests"):
            for path in directory.rglob("*"):
                if path.suffix not in {".py", ".c", ".h"}:
                    continue
                if not (path.name.startswith("test") or "tests" in path.parts or path.name == "kernel_test_sources.py"):
                    continue
                text = path.read_text()
                if path.suffix == ".py":
                    for node in ast.walk(ast.parse(text)):
                        if isinstance(node, ast.Constant) and isinstance(node.value, str):
                            if re.search(r"(?:^|/)(?:userland|third_party|system/rootfs)/", node.value):
                                failures.append(f"{path.relative_to(ROOT)}: {node.value}")
                for include in re.findall(r'^\s*#\s*include\s*"(\.\./[^"]+)"', text, re.M):
                    target = (path.parent / include).resolve()
                    if not target.is_relative_to(ROOT) or not target.is_file():
                        failures.append(f"{path.relative_to(ROOT)}: {include}")
        self.assertEqual(failures, [])

    def test_manifest_covers_kernel_host_suites(self):
        entries = [shlex.split(line, comments=True) for line in
                   (ROOT / "tools/tests/host-suites.list").read_text().splitlines()]
        entries = [entry for entry in entries if entry]
        names = [entry[0] for entry in entries]
        self.assertEqual(len(names), len(set(names)))
        covered = {entry[1] for entry in entries}
        # These suites have prerequisites and dedicated native Make targets.
        native = {"test_abi_layout.py", "test_header_export.py", "test_vdso.py",
                  "test_boot_entry.py", "test_module_layout.py", "test_host.py",
                  "test_test_ownership.py", "test_brand_identity.py"}
        for path in (ROOT / "tools").glob("test_*.py"):
            if path.name not in native:
                self.assertIn(str(path.relative_to(ROOT)), covered)
        for entry in entries:
            self.assertTrue((ROOT / entry[1]).is_file(), entry)

    def test_referenced_local_test_files_exist(self):
        missing = []
        for path in (ROOT / "tools").rglob("*.py"):
            if "test" not in path.name and "tests" not in path.parts:
                continue
            for name in set(re.findall(r"tools/(?:tests/)?[\w./-]+\.(?:py|c|h)", path.read_text())):
                if not (ROOT / name).is_file():
                    missing.append(f"{path.relative_to(ROOT)}: {name}")
        self.assertEqual(missing, [])


if __name__ == "__main__":
    unittest.main()

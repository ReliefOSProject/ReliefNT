#!/usr/bin/env python3
"""Exercise real fault dispatch/gate/cache/paging, not a mirrored fault handler.

Install in tools/ with the two accompanying *_test.c files in tools/tests/.
TMPDIR must point to the parent ReliefOS out/tmp while running in the submodule.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PrivateFaultTransactions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="private-fault-", dir=os.environ["TMPDIR"])
        common = ["cc", "-std=c11", "-pthread", "-O1", "-g", "-Wall", "-Wextra",
                  "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                  "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                  "-Iinclude", "-Iinclude/uapi"]
        cls.dispatch = str(Path(cls.temp.name) / "dispatch")
        cls.sequence = str(Path(cls.temp.name) / "sequence")
        subprocess.run([*common, "tools/tests/private_fault_dispatch_test.c",
                        "arch/x86_64/interrupt/idt.c", "kernel/sched/core.c", "mm/cache/page_cache.c",
                        "fs/vfs/mount_tree.c", "fs/vfs/file.c", "fs/vfs/open.c",
                        "fs/vfs/attributes.c", "fs/vfs/exec_file.c",
                        "-Wl,--wrap=vfs_exec_source_get", "-Wl,--wrap=vfs_exec_source_put",
                        "-o", cls.dispatch],
                       cwd=ROOT, check=True)
        subprocess.run([*common, "tools/tests/paging_sequence_test.c",
                        "arch/x86_64/mm/paging.c", "-o", cls.sequence], cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_private_cow_overlap_and_isolation(self):
        subprocess.run([self.dispatch, "cow"], cwd=ROOT, check=True, timeout=30)

    def test_cow_fault_policy_and_ownership_fallback(self):
        subprocess.run([self.dispatch, "cow-policy"], cwd=ROOT, check=True, timeout=30)

    def test_initial_stack_cow_uses_reader_admission(self):
        subprocess.run([self.dispatch, "stack-cow"], cwd=ROOT, check=True, timeout=30)

    def test_immutable_cache_overlap_and_rollback(self):
        subprocess.run([self.dispatch, "cache"], cwd=ROOT, check=True, timeout=30)

    def test_queued_writer_private_fallback_checkpoint(self):
        subprocess.run([self.dispatch, "writer"], cwd=ROOT, check=True, timeout=30)

    def test_queued_writer_cow_fallback_checkpoint(self):
        subprocess.run([self.dispatch, "writer-cow"], cwd=ROOT, check=True, timeout=30)

    def test_protection_shared_mm_and_kernel_usercopy(self):
        subprocess.run([self.dispatch, "policy"], cwd=ROOT, check=True, timeout=30)

    def test_concurrent_paging_sequence_and_cr3_readers(self):
        subprocess.run([self.sequence], cwd=ROOT, check=True, timeout=30)

    def test_mapping_change_during_cr3_load_remains_visible(self):
        subprocess.run([self.sequence, "reload-race"], cwd=ROOT, check=True, timeout=30)

    def test_remote_reschedule_during_reader_cow(self):
        subprocess.run([self.dispatch, "resched-reader-cow"], cwd=ROOT, check=True, timeout=30)

    def test_remote_reschedule_during_reader_cache_hit(self):
        subprocess.run([self.dispatch, "resched-reader-cache"], cwd=ROOT, check=True, timeout=30)

    def test_remote_reschedule_while_private_fault_waits_for_writer(self):
        subprocess.run([self.dispatch, "resched-writer"], cwd=ROOT, check=True, timeout=30)

    def test_normal_user_return_predicate(self):
        for case in ("safe", "thread-signal", "process-signal", "blocked-signal", "stopped",
                     "exited", "affinity", "bad-frame", "bad-reservation", "pending-load"):
            with self.subTest(case=case):
                subprocess.run([self.dispatch, "return-" + case], cwd=ROOT, check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

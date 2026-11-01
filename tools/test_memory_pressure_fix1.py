#!/usr/bin/env python3
"""Targeted OOM/trap and DEVICE-accounting regression controls."""
import argparse
from contextlib import nullcontext
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(argv):
    """Log the exact compile or fixture execution and propagate its status."""
    print('+ ' + shlex.join(argv), flush=True)
    result = subprocess.run(argv, cwd=ROOT, check=False, timeout=60)
    print('exit=' + str(result.returncode), flush=True)
    result.check_returncode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--case', action='append', choices=['trap-anon', 'trap-cow',
        'vfork', 'late', 'late-vfork', 'late-before-signal', 'policy-cursor', 'wait-cursor', 'group-exit',
        'device', 'lifecycle', 'vfork-fault', 'old-selector', 'old-policy', 'old-protected',
        'old-fault', 'old-fault-integration', 'probe'])
    args = parser.parse_args()
    compiler = shutil.which('clang')
    if not compiler: raise RuntimeError('clang is required')
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
        directory = nullcontext(args.output.resolve())
    else:
        directory = tempfile.TemporaryDirectory(prefix='memory-pressure-fix1-', dir=os.environ.get('TMPDIR'))
    cases = args.case or ['trap-anon', 'trap-cow', 'vfork', 'late', 'late-vfork', 'late-before-signal',
                          'policy-cursor', 'wait-cursor', 'group-exit', 'device', 'lifecycle', 'vfork-fault',
                          'old-selector', 'old-policy', 'old-protected',
                          'old-fault', 'old-fault-integration', 'probe']
    with directory as workdir:
        work = Path(workdir)
        common = [compiler, '-std=c11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
                  '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                  '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
                  '-Iinclude', '-Iinclude/uapi', '-DCONFIG_SWAP=1', '-fno-pie', '-no-pie',
                  '-Wl,--gc-sections']
        binary = work / 'control'
        for case in cases:
            if case.startswith('trap-') or case == 'vfork-fault':
                sources = ['tools/tests/memory_pressure_vfork_fault_test.c' if case == 'vfork-fault'
                           else 'tools/tests/memory_pressure_no_victim_test.c', 'kernel/sched/core.c',
                    'kernel/sched/wait.c', 'kernel/sched/identity.c', 'kernel/sched/oom.c',
                    'kernel/sync/wait.c', 'mm/reclaim/pressure.c', 'mm/reclaim/wait.c',
                    'mm/reclaim/oom.c', 'mm/cache/page_cache.c', 'fs/vfs/mount_tree.c',
                    'fs/vfs/file.c', 'fs/vfs/open.c', 'fs/vfs/attributes.c', 'fs/vfs/exec_file.c',
                    'mm/swap/area.c', 'mm/swap/slot.c', 'mm/swap/paging.c',
                    '-Wl,--wrap=vfs_exec_source_get', '-Wl,--wrap=vfs_exec_source_put']
                if case != 'vfork-fault': sources += ['-Wl,--wrap=memory_oom_advance']
                argument = None if case == 'vfork-fault' else case.removeprefix('trap-')
            elif case in ('vfork', 'late', 'late-vfork', 'late-before-signal', 'policy-cursor', 'wait-cursor', 'group-exit'):
                sources = ['tools/tests/memory_pressure_vfork_regression_test.c',
                           'kernel/sched/identity.c', 'kernel/sched/oom.c']
                argument = case
            elif case == 'device':
                sources = ['tools/tests/memory_pressure_device_score_test.c', 'kernel/exec/usercopy.c',
                    'kernel/sched/identity.c', 'kernel/sched/oom.c', 'mm/reclaim/oom.c',
                    'mm/swap/area.c', 'mm/swap/slot.c', 'mm/swap/drain.c', 'mm/swap/paging.c',
                    '-Wl,--wrap=paging_user_pressure_step']
                argument = None
            elif case == 'lifecycle':
                sources = ['-UCONFIG_SWAP', 'tools/tests/memory_pressure_oom_lifecycle_test.c', 'kernel/signal/signal_queue.c',
                           'kernel/sched/oom.c', 'mm/reclaim/oom.c']
                argument = None
            elif case in ('old-fault', 'old-fault-integration'):
                sources = ['tools/tests/memory_pressure_fault_integration_test.c' if case.endswith('integration')
                           else 'tools/tests/memory_pressure_fault_test.c', 'kernel/sched/core.c']
                if case.endswith('integration'):
                    sources += ['kernel/sched/wait.c', 'kernel/sched/oom.c', 'kernel/sync/wait.c',
                                'mm/reclaim/pressure.c', 'mm/reclaim/wait.c']
                sources += ['mm/cache/page_cache.c', 'fs/vfs/mount_tree.c', 'fs/vfs/file.c',
                            'fs/vfs/open.c', 'fs/vfs/attributes.c', 'fs/vfs/exec_file.c',
                            'mm/swap/area.c', 'mm/swap/slot.c', 'mm/swap/paging.c',
                            '-Wl,--wrap=vfs_exec_source_get', '-Wl,--wrap=vfs_exec_source_put']
                argument = None
            elif case == 'probe':
                sources = ['-D_GNU_SOURCE=1', 'tools/tests/memory_pressure_guest_probe.c']
                argument = '1048576'
            else:
                sources = ['tools/tests/memory_pressure_oom_test.c', 'kernel/sched/identity.c',
                           'kernel/sched/oom.c', 'mm/reclaim/oom.c', 'mm/reclaim/stats.c']
                argument = {'old-selector': 'selection', 'old-policy': 'policy',
                            'old-protected': 'protected'}[case]
            run([*common, *sources, '-o', str(binary)])
            run([str(binary), *([argument] if argument else [])])
            if case == 'probe':
                from test_memory_pressure import run_held_probe
                run_held_probe(binary)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Run focused eligibility/continuous-change OOM regression controls."""
import argparse
from contextlib import nullcontext
import os
from pathlib import Path
import hashlib
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(argv):
    """Log one compiler or fixture command and propagate its actual status."""
    print('+ ' + shlex.join(argv), flush=True)
    result = subprocess.run(argv, cwd=ROOT, check=False, timeout=60)
    print('exit=' + str(result.returncode), flush=True)
    result.check_returncode()


def main():
    """Compile and run only the selected production-linked host controls."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--case', action='append', choices=['verify-exec','primary-exec','complete-only',
        'protected-exec','exit','vfork-exit','wait-churn','device','device-no-victim','membership','stable'])
    parser.add_argument('--historical-oom', type=Path)
    args = parser.parse_args()
    compiler = shutil.which('clang')
    if not compiler: raise RuntimeError('clang is required')
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
        directory = nullcontext(args.output.resolve())
    else:
        directory = tempfile.TemporaryDirectory(prefix='memory-pressure-fix2-',dir=os.environ.get('TMPDIR'))
    cases = args.case or ['verify-exec','primary-exec','complete-only','protected-exec','exit','vfork-exit','wait-churn',
                          'device','device-no-victim','membership','stable']
    with directory as workdir:
        binary=Path(workdir)/'control'
        common=[compiler,'-std=c11','-O1','-g','-pthread','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                '-ffunction-sections','-fdata-sections','-Iinclude','-Iinclude/uapi',
                '-DCONFIG_SWAP=1','-fno-pie','-no-pie','-Wl,--gc-sections']
        for case in cases:
            if case in ('verify-exec','primary-exec','complete-only','protected-exec'):
                sources=['tools/tests/memory_pressure_fix2_eligibility_test.c',
                    'kernel/sched/identity.c','kernel/sched/oom.c','kernel/sched/exec.c','kernel/sched/clone.c']
                argument=case
            elif case in ('exit','vfork-exit','wait-churn'):
                sources=['-UCONFIG_SWAP','tools/tests/memory_pressure_fix2_exit_test.c',
                         'kernel/signal/signal_queue.c','kernel/sched/oom.c']
                argument=case if case!='exit' else None
            else:
                sources=['tools/tests/memory_pressure_fix2_churn_test.c','kernel/exec/usercopy.c',
                    'kernel/sched/identity.c','kernel/sched/oom.c','mm/reclaim/oom.c',
                    'mm/swap/area.c','mm/swap/slot.c','mm/swap/drain.c','mm/swap/paging.c',
                    '-Wl,--wrap=paging_user_pressure_step']
                argument=case
            if args.historical_oom:
                if case not in ('verify-exec','primary-exec','complete-only','protected-exec',
                                'exit','vfork-exit','wait-churn'):
                    raise ValueError('historical OOM source is only supported by selector-including controls')
                original=args.historical_oom.resolve()
                shadow=Path(workdir)/'historical-oom';shadow.mkdir(exist_ok=False)
                for name in ('oom.c','internal.h'):
                    content=(original.parent/name).read_bytes()
                    (shadow/name).write_bytes(content)
                    print('captured '+str(original.parent/name)+' sha256='+hashlib.sha256(content).hexdigest(),flush=True)
                sources.insert(0,'-DMEMORY_PRESSURE_FIX2_OOM_SOURCE="'+str(shadow/'oom.c')+'"')
            run([*common,*sources,'-o',str(binary)])
            run([str(binary),*([argument] if argument else [])])


if __name__ == '__main__':
    main()

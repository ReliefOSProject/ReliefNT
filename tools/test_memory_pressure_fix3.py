#!/usr/bin/env python3
"""Run focused published shared-MM retirement regression controls."""
import argparse
from contextlib import nullcontext
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(argv):
    """Log an actual command and propagate its exit status."""
    print('+ ' + shlex.join(argv), flush=True)
    result = subprocess.run(argv, cwd=ROOT, check=False, timeout=60)
    print('exit=' + str(result.returncode), flush=True)
    result.check_returncode()


def main():
    """Compile real lifecycle paths and run the requested focused controls."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--case', action='append', choices=['release', 'churn', 'late-eligible',
                        'late-protected', 'late-vfork', 'exited-sibling'])
    args = parser.parse_args()
    compiler = shutil.which('clang')
    if not compiler:
        raise RuntimeError('clang is required')
    compiler = str(Path(compiler).resolve())
    print('compiler=' + compiler + ' sha256=' + hashlib.sha256(Path(compiler).read_bytes()).hexdigest(), flush=True)
    run([compiler, '--version'])
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
        directory = nullcontext(args.output.resolve())
    else:
        directory = tempfile.TemporaryDirectory(prefix='memory-pressure-fix3-', dir=os.environ.get('TMPDIR'))
    with directory as workdir:
        binary = Path(workdir) / 'control'
        for case in args.case or ['release', 'churn', 'late-eligible', 'late-protected',
                                  'late-vfork', 'exited-sibling']:
            source = ('tools/tests/memory_pressure_fix3_late_test.c'
                      if case.startswith(('late-', 'exited-'))
                      else 'tools/tests/memory_pressure_fix3_publication_test.c')
            run([compiler, '-std=c11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                 '-ffunction-sections', '-fdata-sections', '-Iinclude', '-Iinclude/uapi', '-UCONFIG_SWAP',
                 '-fno-pie', '-no-pie', '-Wl,--gc-sections', source,
                 'kernel/signal/signal_queue.c', 'kernel/sched/oom.c', '-o', str(binary)])
            run([str(binary), case])


if __name__ == '__main__':
    main()

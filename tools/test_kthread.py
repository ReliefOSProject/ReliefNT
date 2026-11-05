#!/usr/bin/env python3
"""Run retained-stack kernel threads through the native x86-64 switch."""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
base=Path(os.environ.get('TMPDIR',str(ROOT/'out/tmp')));base.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='kthread-',dir=base) as temporary:
    target=Path(temporary)/'kthread'
    core=ROOT/'kernel/kthread/core.c'
    sources=[str(core),'kernel/kthread/dispatch.c','kernel/workqueue/core.c','arch/x86_64/cpu/kthread.S','kernel/sync/lock.c'] if core.exists() else ['-DKTHREAD_MISSING']
    subprocess.run(['clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
        '-DCONFIG_MODULES','-Iinclude','-Iinclude/uapi','tools/tests/kthread_test.c',*sources,'-o',str(target)],cwd=ROOT,check=True)
    subprocess.run([target],cwd=ROOT,check=True,timeout=20)

with tempfile.TemporaryDirectory(prefix='kthread-storage-',dir=base) as temporary:
    target=Path(temporary)/'storage'
    sources=['block/core/storage_state.c'] if (ROOT/'include/reliefnt/storage_context.h').exists() else ['-DSTORAGE_CONTEXT_MISSING']
    subprocess.run(['clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iinclude','-Iinclude/uapi',
        'tools/tests/kthread_storage_test.c',*sources,'-o',str(target)],cwd=ROOT,check=True)
    subprocess.run([target],cwd=ROOT,check=True,timeout=20)

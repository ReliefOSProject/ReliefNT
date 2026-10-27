#!/usr/bin/env python3
"""Exercise production file-page cache admission and ownership with sanitizers."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
def main():
    compiler=shutil.which('clang') or shutil.which('cc')
    if compiler is None:raise RuntimeError('a C compiler is required')
    temporary=Path(os.environ.get("TMPDIR", str(ROOT / "out" / "tmp")));temporary.mkdir(parents=True,exist_ok=True)
    source=ROOT/'mm/cache/page_cache.c'
    print('SOURCE page_cache.c sha256='+hashlib.sha256(source.read_bytes()).hexdigest(),flush=True)
    with tempfile.TemporaryDirectory(prefix='page-cache-',dir=temporary) as directory:
        env={**os.environ,'TMPDIR':str(temporary)}
        for name,sources in (
            ('page_cache_growth',['kernel/sync/lock.c']),
            ('elf_file_page',['fs/vfs/mount_tree.c','fs/vfs/file.c','fs/vfs/attributes.c','fs/vfs/exec_file.c']),
        ):
            binary=Path(directory)/name
            subprocess.run([compiler,'-std=c11','-pthread','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                '-Iinclude','-Iinclude','-Iinclude/uapi',
                '-Iinclude',f'tools/tests/{name}_test.c',
                'mm/cache/page_cache.c',*sources,
                '-o',str(binary)],cwd=ROOT,env=env,check=True)
            subprocess.run([str(binary)],cwd=ROOT,env=env,check=True,timeout=45)
if __name__=='__main__':main()

#!/usr/bin/env python3
"""Compile native module metadata with the production x86-64 address model."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <reliefnt/module.h>
#include <reliefnt/moduleparam.h>
#include <reliefnt/export.h>
static bool enabled;
static int count;
static char *label;
module_param(enabled, bool, 0644);
module_param_named(retries, count, int, 0444);
module_param(label, charp, 0);
MODULE_PARM_DESC(retries, "Retry count");
int sample_export(int value) { return value + count; }
EXPORT_SYMBOL(sample_export);
static int sample_init(void) { return enabled ? 0 : -1; }
static void sample_exit(void) { }
module_init(sample_init);
module_exit(sample_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT native module fixture");
MODULE_ALIAS("reliefnt:test");
'''


class ModuleHeaders(unittest.TestCase):
    def test_builtin_owner_operations_without_modules(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-module-disabled-") as directory:
            work=Path(directory)
            source=work / "builtin.c"
            source.write_text('''#include <assert.h>
#include <reliefnt/module.h>
static void release(void *unused) { (void)unused; }
int main(void) {
    struct module impossible={0};
    assert(try_module_get(0) && !try_module_get(&impossible));
    module_put(0);
    assert(module_execution_get(0) && !module_execution_get(&impossible));
    module_execution_put(0);
    assert(!module_add_resource(0,0,release,0));
    assert(module_add_resource(&impossible,0,release,0)==-38);
}
''')
            binary=work / "builtin"
            subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I"+str(ROOT / "include"), str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_module_and_builtin_objects(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-module-headers-") as directory:
            work = Path(directory)
            source = work / "fixture.c"
            source.write_text(SOURCE)
            for external in (False, True):
                target = work / ("external.o" if external else "builtin.o")
                command = [os.environ.get("CC", "clang"), "--target=x86_64-elf",
                           "-std=c11", "-ffreestanding", "-fno-pic", "-fno-pie",
                           "-mno-red-zone", "-mcmodel=large", "-Wall", "-Wextra", "-Werror",
                           "-DKBUILD_MODNAME=\"sample\"", "-I" + str(ROOT / "include"),
                           "-DCONFIG_MODULES=1", "-c", str(source), "-o", str(target)]
                if external:
                    command.append("-DMODULE=1")
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                sections = subprocess.check_output(["readelf", "-SW", str(target)], text=True)
                self.assertIn(".modinfo", sections)
                self.assertIn("__param", sections)
                self.assertIn(".export_symbol", sections)
                symbols = subprocess.check_output(["nm", "-g", str(target)], text=True)
                if external:
                    self.assertIn(" init_module", symbols)
                    self.assertIn(" cleanup_module", symbols)
                else:
                    self.assertNotIn(" init_module", symbols)
                    self.assertIn(".reliefnt.initcall", sections)
                strings = subprocess.check_output(["readelf", "-p", ".modinfo", str(target)], text=True)
                self.assertIn("license=Apache-2.0", strings)
                self.assertIn("alias=reliefnt:test", strings)
                self.assertIn("parmtype=retries:int", strings)


if __name__ == "__main__":
    unittest.main()

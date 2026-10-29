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
    def test_final_exports_keep_linux_tool_visible_symbols(self):
        # Removing the per-export labels breaks depmod's symbol index even
        # though the kernel can still load the same pointer-table bytes.
        with tempfile.TemporaryDirectory(prefix="reliefnt-export-metadata-") as directory:
            work = Path(directory)
            source = work / "exports.mod.c"
            source.write_text('''#include <linux/export-internal.h>
int ordinary_function(void) { return 42; }
int restricted_function(void) { return 7; }
int ordinary_data = 3;
int restricted_data = 5;
KSYMTAB_FUNC(ordinary_function, "", "");
KSYMTAB_FUNC(restricted_function, "_gpl", "test_namespace");
KSYMTAB_DATA(ordinary_data, "", "");
KSYMTAB_DATA(restricted_data, "_gpl", "test_namespace");
''')
            target = work / "exports.mod.o"
            subprocess.run(["clang", "--target=x86_64-elf", "-ffreestanding",
                            "-fno-pic", "-fno-pie", "-mcmodel=large",
                            "-I" + str(ROOT / "scripts/module-include"),
                            "-c", str(source), "-o", str(target)], check=True)
            symbols = subprocess.check_output(["readelf", "-sW", str(target)], text=True)
            names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
            for name in ("ordinary_function", "restricted_function",
                         "ordinary_data", "restricted_data"):
                with self.subTest(export=name):
                    self.assertIn("__ksymtab_" + name, names,
                                  "depmod must be able to identify every exported function/data item")
                    self.assertIn("__kstrtab_" + name, names)
                    self.assertIn("__kstrtabns_" + name, names)

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

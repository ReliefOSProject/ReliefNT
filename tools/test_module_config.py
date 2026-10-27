#!/usr/bin/env python3
"""Exercise native Kconfig module semantics without the OS configuration."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def run_config(command, **kwargs):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, **kwargs)
    if result.returncode:
        raise AssertionError(result.stdout)
    return result


class ModuleConfiguration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="reliefnt-module-config-")
        cls.directory = Path(cls.work.name)
        cls.output = cls.directory / "build"
        subprocess.run(["make", "-s", "-C", str(ROOT), "O=" + str(cls.output),
                        "defconfig"], check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        cls.conf = cls.output / "scripts/kconfig/conf"

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def test_default_is_builtin_and_loader_enabled(self):
        text = (self.output / ".config").read_text()
        self.assertIn("CONFIG_MODULES=y\n", text)
        self.assertIn("CONFIG_MODULE_UNLOAD=y\n", text)
        self.assertNotIn("=m\n", text)
        for symbol in ("DRM", "SND", "FAT_FS", "EXFAT_FS", "EXT2_FS", "EXT4_FS", "EXT_FS_COMMON", "ISO9660_FS"):
            self.assertIn("CONFIG_" + symbol + "=y\n", text)

    def test_native_dependency_and_disabled_modules(self):
        fixture = self.directory / "Kconfig.fixture"
        fixture.write_text(f'source "{ROOT}/kernel/module/Kconfig"\n'
                           'config TEST_PROVIDER\n tristate "provider"\n'
                           'config TEST_CONSUMER\n tristate "consumer"\n'
                           ' depends on TEST_PROVIDER\n')
        config = self.directory / "fixture.config"
        env = {**os.environ, "srctree": str(ROOT), "KCONFIG_CONFIG": str(config),
               "KCONFIG_AUTOCONFIG": str(self.directory / "fixture/include/config/auto.conf"),
               "KCONFIG_AUTOHEADER": str(self.directory / "fixture/include/generated/autoconf.h")}
        config.write_text("CONFIG_MODULES=y\nCONFIG_TEST_PROVIDER=m\nCONFIG_TEST_CONSUMER=y\n")
        run_config([str(self.conf), "--olddefconfig", str(fixture)], cwd=self.directory, env=env)
        text = config.read_text()
        self.assertIn("CONFIG_TEST_PROVIDER=m\n", text)
        self.assertIn("CONFIG_TEST_CONSUMER=m\n", text)
        config.write_text(text.replace("CONFIG_MODULES=y", "# CONFIG_MODULES is not set"))
        run_config([str(self.conf), "--olddefconfig", str(fixture)], cwd=self.directory, env=env)
        text = config.read_text()
        self.assertNotIn("=m\n", text)
        self.assertNotIn("CONFIG_MODULE_UNLOAD=y", text)

    def test_production_driver_tristates(self):
        output = self.directory / "all-drivers"
        run_config(["make", "-s", "-C", str(ROOT), "O=" + str(output), "allmodconfig"])
        text = (output / ".config").read_text()
        for symbol in ("DRM", "SND", "E1000", "SND_INTEL8X0", "SND_ENS1371", "SND_HDA_INTEL",
                       "MOUSE_PS2", "SERIAL_8250", "USB_UHCI_HCD", "SATA_AHCI", "PATA_IDE", "BLK_DEV_NVME"):
            self.assertIn("CONFIG_" + symbol + "=m\n", text)

    def test_sound_core_bounds_native_hardware_tristates(self):
        output=self.directory / "sound-core";output.mkdir()
        hardware=("SND_INTEL8X0","SND_ENS1371","SND_HDA_INTEL")
        for modules,core,expected in (("y","m","m"),("y","n",None),("y","y","y"),("n","m","y")):
            with self.subTest(modules=modules,core=core):
                (output / ".config").write_text("CONFIG_MODULES="+modules+"\nCONFIG_SOUND=y\nCONFIG_SND="+core+"\n"+
                    "".join("CONFIG_"+name+"=y\n" for name in hardware))
                run_config(["make","-s","-C",str(ROOT),"O="+str(output),"olddefconfig"])
                text=(output / ".config").read_text()
                for name in hardware:
                    if expected:self.assertIn("CONFIG_"+name+"="+expected+"\n",text)
                    else:
                        self.assertNotIn("CONFIG_"+name+"=y\n",text)
                        self.assertNotIn("CONFIG_"+name+"=m\n",text)
                if modules=="n":self.assertNotIn("=m\n",text)

    def test_graphics_core_tristate(self):
        output=self.directory / "graphics-core";output.mkdir()
        for modules,requested,expected in (("y","y","y"),("y","m","m"),("y","n",None),("n","m","y")):
            with self.subTest(modules=modules,requested=requested):
                (output / ".config").write_text("CONFIG_MODULES="+modules+"\nCONFIG_DRM="+requested+"\n")
                run_config(["make","-s","-C",str(ROOT),"O="+str(output),"olddefconfig"])
                text=(output / ".config").read_text()
                if expected:self.assertIn("CONFIG_DRM="+expected+"\n",text)
                else:self.assertNotRegex(text,r"CONFIG_DRM=[ym]\n")
                if modules=="n":self.assertNotIn("=m\n",text)

    def test_foreign_symbols_removed_and_sync_is_idempotent(self):
        output = self.directory / "foreign"
        output.mkdir()
        (output / ".config").write_text("CONFIG_RELIEFOS_COMPONENT_APP_FOREIGN_BUILD=y\n"
                                        "CONFIG_QEMU_SMP_CPUS=32\nCONFIG_MODULES=y\n")
        command = ["make", "-s", "-C", str(ROOT), "O=" + str(output), "olddefconfig"]
        subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        before = (output / ".config").read_bytes()
        self.assertNotIn(b"CONFIG_RELIEFOS_", before)
        self.assertNotIn(b"CONFIG_QEMU_", before)
        self.assertIn(b"CONFIG_MODULES=y", before)
        subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(before, (output / ".config").read_bytes())

    def test_filesystem_shared_dependency(self):
        output = self.directory / "filesystems"
        run_config(["make", "-s", "-C", str(ROOT), "O=" + str(output), "allmodconfig"])
        text = (output / ".config").read_text()
        symbols = ("FAT_FS", "EXFAT_FS", "EXT2_FS", "EXT4_FS", "EXT_FS_COMMON", "ISO9660_FS")
        for symbol in symbols:
            self.assertIn("CONFIG_" + symbol + "=m\n", text)
        for ext2, ext4, common in (("y", "m", "y"), ("m", "y", "y"), ("m", "n", "m"), ("n", "n", "n")):
            (output / ".config").write_text("CONFIG_MODULES=y\nCONFIG_EXT2_FS=" + ext2 +
                "\nCONFIG_EXT4_FS=" + ext4 + "\n")
            run_config(["make", "-s", "-C", str(ROOT), "O=" + str(output), "olddefconfig"])
            text = (output / ".config").read_text()
            if common == "n":
                self.assertNotIn("CONFIG_EXT_FS_COMMON=y", text)
                self.assertNotIn("CONFIG_EXT_FS_COMMON=m", text)
            else:
                self.assertIn("CONFIG_EXT_FS_COMMON=" + common + "\n", text)


if __name__ == "__main__":
    unittest.main()

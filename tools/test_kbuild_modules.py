#!/usr/bin/env python3
"""Exercise original Kbuild/modpost/genksyms, never a hand-written .ko linker."""
from pathlib import Path
import os
import shutil
import shlex
import subprocess
import tempfile
import unittest
import hashlib
ROOT = Path(__file__).resolve().parents[1]


class KbuildModules(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="reliefnt-kbuild-modules-")
        cls.directory = Path(cls.work.name)
        cls.output = cls.directory / "build"

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def make(self, *arguments, success=True):
        command = ["make", "-s", "-j4", "-C", str(ROOT), "O=" + str(self.output), *arguments]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout[-12000:])
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout[-12000:])
        return result

    def check_replaced_linker(self):
        wrapper = self.directory / "target-ld"
        log = self.directory / "target-ld.log"
        real = shutil.which("ld.lld")
        self.assertIsNotNone(real)
        for revision in (1, 2):
            wrapper.write_text("#!/bin/sh\n# tool revision " + str(revision) + "\n" +
                               "printf '%s\\n' \"$*\" >> " + shlex.quote(str(log)) + "\n" +
                               "exec " + shlex.quote(real) + " \"$@\"\n")
            wrapper.chmod(0o755)
            log.write_text("")
            self.make("modules", "LD=" + str(wrapper))
            if revision == 2:
                targets = []
                for line in log.read_text().splitlines():
                    arguments = shlex.split(line)
                    if "-o" in arguments:
                        targets.append(arguments[arguments.index("-o") + 1].removeprefix("./"))
                self.assertIn("tools/tests/kmodules/test_provider.o", targets,
                              "same-path linker replacement must relink composite modules: " + repr(targets))
                self.assertIn("vmlinux.o", targets)

    def check_relocated_linker(self):
        real = shutil.which("ld.lld")
        self.assertIsNotNone(real)
        content = "#!/bin/sh\nexec " + shlex.quote(real) + " \"$@\"\n"
        identities = []
        for location in ("toolchain-one", "relocated-toolchain"):
            directory = self.directory / location
            directory.mkdir()
            wrapper = directory / "ld"
            wrapper.write_text(content)
            wrapper.chmod(0o755)
            self.make("modules_prepare", "LD=" + str(wrapper))
            identities.append((self.output / "include/config/module.abi").read_bytes())
        self.assertEqual(identities[0], identities[1],
                         "relocating identical tool bytes/arguments must preserve native module ABI identity")

    def check_prepared_sdk(self):
        prefix=self.directory / "sdk-stage"
        self.make("modules_sdk", "INSTALL_MOD_PATH="+str(prefix))
        release=(self.output / "include/config/kernel.release").read_text().strip()
        sdk=prefix / "usr/src" / ("reliefnt-"+release)
        self.assertTrue((sdk / "Makefile").is_file())
        self.assertEqual((sdk / "Module.symvers").read_bytes(),(self.output / "Module.symvers").read_bytes())
        self.assertFalse((sdk / "kernel").exists(),"prepared headers must not need kernel implementation sources")
        relocated=self.directory / "relocated-sdk"
        shutil.copytree(sdk,relocated)
        external=self.directory / "sdk-external";external.mkdir()
        (external / "Kbuild").write_text("obj-m += sdk_sample.o\n"
            "hostprogs-always-y := sdk_gen\nalways-y += sdk_generated.h\ntargets += sdk_generated.h\n"
            "quiet_cmd_sdk_gen = GEN     $@\ncmd_sdk_gen = $(obj)/sdk_gen > $@\n"
            "$(obj)/sdk_generated.h: $(obj)/sdk_gen FORCE\n\t$(call if_changed,sdk_gen)\n"
            "$(obj)/sdk_sample.o: $(obj)/sdk_generated.h\n")
        (external / "sdk_gen.c").write_text('#include <stdio.h>\n'
            'int main(void) { puts("#define SDK_GENERATED 7");return 0; }\n')
        (external / "sdk_sample.c").write_text('#include <reliefnt/module.h>\n#include <reliefnt/console.h>\n'
            '#include "sdk_generated.h"\n'
            'static int start(void) { console_log(CONSOLE_LOG_INFO,"sdk sample\\n");return SDK_GENERATED-7; }\nstatic void stop(void) {}\n'
            'module_init(start);\nmodule_exit(stop);\nMODULE_LICENSE("Apache-2.0");\n')
        snapshot={str(p.relative_to(relocated)):hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in relocated.rglob("*") if p.is_file()}
        modes={p:p.stat().st_mode & 0o777 for p in [relocated,*relocated.rglob("*")]}
        try:
            for path,mode in modes.items():path.chmod(mode & ~0o222)
            command=["make","-s","-j4","-C",str(relocated),"M="+str(external)]
            built=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            self.assertEqual(built.returncode,0,built.stdout[-10000:])
            self.assertTrue((external / "sdk_sample.ko").is_file())
            self.assertEqual((external / "sdk_generated.h").read_text(),"#define SDK_GENERATED 7\n")
            info=subprocess.check_output(["readelf","-p",".modinfo",str(external / "sdk_sample.ko")],text=True)
            self.assertIn(release,info)
            self.assertIn((self.output / "include/config/module.abi").read_text().strip(),info)
            install=self.directory / "sdk-module-stage"
            installed=subprocess.run([*command,"modules_install","INSTALL_MOD_PATH="+str(install)],
                stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            self.assertEqual(installed.returncode,0,installed.stdout[-10000:])
            self.assertTrue((install / "lib/modules" / release / "updates/sdk_sample.ko").is_file())
            bad_tool=self.directory / "other-cc"
            bad_tool.write_text("#!/bin/sh\nexec "+shlex.quote(shutil.which("clang"))+" \"$@\"\n")
            bad_tool.chmod(0o755)
            refused=subprocess.run([*command,"CC="+str(bad_tool)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            self.assertNotEqual(refused.returncode,0)
            self.assertIn("toolchain identity",refused.stdout)
            self.assertEqual(snapshot,{str(p.relative_to(relocated)):hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in relocated.rglob("*") if p.is_file()})
        finally:
            for path,mode in modes.items():path.chmod(mode)
        tampered=self.directory / "tampered-sdk";shutil.copytree(sdk,tampered)
        header=tampered / "include/reliefnt/module.h";header.write_text(header.read_text()+"\n/* changed after export */\n")
        refused=subprocess.run(["make","-s","-C",str(tampered),"M="+str(external)],
            stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        with self.subTest("prepared SDK integrity"):
            self.assertNotEqual(refused.returncode,0,"edited prepared headers must not retain an unchecked ABI claim")
            self.assertIn("modified prepared SDK",refused.stdout)
        alias_prefix=self.directory / "sdk-alias-stage"
        alias_tree=alias_prefix / "user-data";shutil.copytree(sdk,alias_tree)
        alias=alias_prefix / "usr/src" / ("reliefnt-"+release);alias.parent.mkdir(parents=True)
        alias.symlink_to(alias_tree,target_is_directory=True)
        with self.subTest("prepared SDK destination ownership"):
            refused=self.make("modules_sdk","INSTALL_MOD_PATH="+str(alias_prefix),success=False)
            self.assertIn("symlink SDK destination",refused.stdout)
        merged=self.directory / "sdk-usrmerge-stage";(merged / "usr/lib").mkdir(parents=True)
        (merged / "lib").symlink_to("usr/lib",target_is_directory=True)
        self.make("modules_sdk","INSTALL_MOD_PATH="+str(merged))
        with self.subTest("usrmerge build link"):
            build_link=merged / "lib/modules" / release / "build"
            self.assertTrue(build_link.is_dir(),"build link must resolve through a standard lib -> usr/lib layout")
            self.assertEqual(build_link.resolve(),(merged / "usr/src" / ("reliefnt-"+release)).resolve())

    def test_native_module_products_and_transition(self):
        self.make("modules_prepare")
        self.assertTrue((self.output / "scripts/mod/modpost").is_file())
        self.assertTrue((self.output / "scripts/genksyms/genksyms").is_file())
        self.assertTrue((self.output / "include/generated/module_abi.h").is_file())
        config = self.output / ".config"
        original = config.read_text()
        self.assertNotIn("=m\n", original)
        self.assertIn("CONFIG_E1000=y", original)
        self.assertIn("CONFIG_SND_HDA_INTEL=y", original)
        config.write_text(original.replace("# CONFIG_MODULE_TESTS is not set", "CONFIG_MODULE_TESTS=y") +
                          "\nCONFIG_MODULE_TEST_PROVIDER=m\nCONFIG_MODULE_TEST_CONSUMER=m\n")
        self.make("olddefconfig", "modules")
        provider = self.output / "tools/tests/kmodules/test_provider.ko"
        consumer = self.output / "tools/tests/kmodules/consumer.ko"
        self.assertTrue(provider.is_file())
        self.assertTrue(consumer.is_file())
        exports = subprocess.check_output(["nm", str(provider)], text=True)
        self.assertIn(" __ksymtab_provided_value\n", exports,
                      "the final linked .ko must retain depmod's export marker")
        self.check_replaced_linker()
        self.check_relocated_linker()
        info = subprocess.check_output(["readelf", "-p", ".modinfo", str(consumer)], text=True)
        self.assertIn("depends=test_provider", info)
        symbols = (self.output / "Module.symvers").read_text()
        self.assertRegex(symbols, r"0x[0-9a-f]{8}\s+provided_value\s+tools/tests/kmodules/test_provider")
        sections = subprocess.check_output(["readelf", "-SW", str(consumer)], text=True)
        self.assertIn("__versions", sections)
        stage = self.directory / "stage"
        self.make("modules_install", "INSTALL_MOD_PATH=" + str(stage))
        release = (self.output / "include/config/kernel.release").read_text().strip()
        modules = stage / "lib/modules" / release
        self.assertTrue((modules / "kernel/tools/tests/kmodules/consumer.ko").is_file())
        user = modules / "kernel/user-kept.ko"
        user.write_bytes(b"user owned")
        # A native y/m/n transition must remove only previously managed products.
        config.write_text(config.read_text().replace("CONFIG_MODULE_TEST_CONSUMER=m", "# CONFIG_MODULE_TEST_CONSUMER is not set")
                          .replace("CONFIG_MODULE_TEST_PROVIDER=m", "CONFIG_MODULE_TEST_PROVIDER=y"))
        self.make("olddefconfig", "modules", "modules_install", "INSTALL_MOD_PATH=" + str(stage))
        self.assertEqual(user.read_bytes(), b"user owned")
        self.assertTrue((modules / "kernel/tools/tests/kmodules/consumer.ko").is_file())
        newer = (self.output / "include/config/kernel.release").read_text().strip()
        self.assertNotEqual(newer, release)
        self.assertFalse((stage / "lib/modules" / newer / "kernel/tools/tests/kmodules/consumer.ko").exists())
        self.assertFalse(consumer.exists())
        self.assertFalse((self.output / "modules.order").read_text().strip())
        builtin = (self.output / "modules.builtin").read_text()
        self.assertIn("tools/tests/kmodules/test_provider.ko", builtin)
        # External M= modules go through the same modpost pipeline. A missing
        # export must fail, rather than silently leaving a runtime relocation.
        external = self.directory / "external"
        external.mkdir()
        (external / "Kbuild").write_text("obj-m += external.o\n")
        source = external / "external.c"
        source.write_text('#include <reliefnt/module.h>\n'
                          'extern int reliefnt_missing_export(void);\n'
                          'static int start(void) { return reliefnt_missing_export(); }\n'
                          'module_init(start);\nMODULE_LICENSE("Apache-2.0");\n')
        failed = self.make("modules", "M=" + str(external), success=False)
        self.assertIn("reliefnt_missing_export", failed.stdout)
        source.write_text('#include <reliefnt/module.h>\n'
                          'static int start(void) { return 0; }\n'
                          'static void stop(void) {}\n'
                          'module_init(start);\nmodule_exit(stop);\nMODULE_LICENSE("Apache-2.0");\n')
        self.make("M="+str(external))
        self.assertTrue((external / "external.ko").is_file())
        self.make("modules_install", "M=" + str(external), "INSTALL_MOD_PATH=" + str(stage))
        self.assertTrue((external / "external.ko").is_file())
        self.assertTrue((stage / "lib/modules" / newer / "updates/external.ko").is_file())
        self.check_prepared_sdk()
        # Preview mode must not even parse upstream modinst's rm/mkdir actions.
        snapshot = {str(p.relative_to(stage)): p.read_bytes() for p in stage.rglob("*") if p.is_file()}
        self.make("-n", "modules_install", "INSTALL_MOD_PATH=" + str(stage))
        self.assertEqual(snapshot, {str(p.relative_to(stage)): p.read_bytes() for p in stage.rglob("*") if p.is_file()})
        source.unlink()
        self.make("modules", "M=" + str(external), success=False)
        (external / "Kbuild").write_text("# no external modules selected\n")
        self.make("modules_install", "M=" + str(external), "INSTALL_MOD_PATH=" + str(stage))
        self.assertFalse((external / "external.ko").exists())
        self.assertFalse((stage / "lib/modules" / newer / "updates/external.ko").exists())
        # The fully disabled configuration has valid builtin metadata and no
        # module products. A second O cannot disturb the first output's files.
        saved_provider = (self.output / "generated/system/kernel.sys").read_bytes()
        second = self.directory / "disabled"
        self.make("defconfig", "O=" + str(second))
        disabled_config = second / ".config"
        disabled_config.write_text(disabled_config.read_text().replace("CONFIG_MODULES=y", "# CONFIG_MODULES is not set"))
        self.make("olddefconfig", "modules_install", "O=" + str(second), "INSTALL_MOD_PATH=" + str(stage))
        self.assertEqual(saved_provider, (self.output / "generated/system/kernel.sys").read_bytes())
        self.assertFalse((second / "modules.order").read_text().strip())
        self.make("modules", "O="+str(second), "M="+str(external), success=False)
        # Building the same source archive without .git must retain the ABI
        # identity. Offline dependencies remain explicit, shared read-only.
        archive = self.directory / "archive"
        def archive_ignored(directory, names):
            excluded = {".git", "__pycache__"}
            if Path(directory) == ROOT:
                excluded.update(("out", "cache", ".superpowers"))
            return excluded.intersection(names)
        shutil.copytree(ROOT, archive, ignore=archive_ignored)
        archived_out = self.directory / "archived-build"
        self.make("defconfig", "O=" + str(archived_out), "-C", str(archive))
        (archived_out / ".config").write_text(disabled_config.read_text())
        self.make("olddefconfig", "modules", "O=" + str(archived_out), "-C", str(archive),
                  "RELIEFNT_CACHE=" + str(ROOT / "cache/downloads"))
        self.assertEqual((second / "include/config/module.abi").read_bytes(),
                         (archived_out / "include/config/module.abi").read_bytes())
        allmods = self.directory / "allmod"
        self.make("allmodconfig", "modules", "O=" + str(allmods))
        all_config = (allmods / ".config").read_text()
        self.assertIn("CONFIG_MODULE_TEST_PROVIDER=m", all_config)
        self.assertIn("CONFIG_MODULE_TEST_CONSUMER=m", all_config)
        self.assertIn("CONFIG_E1000=m", all_config)
        self.assertTrue((allmods / "drivers/net/e1000/e1000.ko").is_file())
        self.assertIn("CONFIG_SND_HDA_INTEL=m", all_config)
        self.assertTrue((allmods / "drivers/sound/hda/hda.ko").is_file())
        self.assertIn("CONFIG_SND_INTEL8X0=m", all_config)
        self.assertIn("CONFIG_SND_ENS1371=m", all_config)
        self.assertTrue((allmods / "drivers/sound/ac97/ac97.ko").is_file())
        self.assertTrue((allmods / "drivers/sound/es1371/es1371.ko").is_file())
        symbols = subprocess.check_output(["llvm-nm", str(allmods / "generated/system/kernel.sys")], text=True)
        self.assertNotIn(" e1000_driver_module", symbols)
        self.assertNotIn(" hda_driver_module", symbols)
        self.assertNotIn(" ac97_driver_module", symbols)
        self.assertNotIn(" es1371_driver_module", symbols)
        self.assertIn("CONFIG_MOUSE_PS2=m", all_config)
        self.assertIn("CONFIG_SERIAL_8250=m", all_config)
        self.assertTrue((allmods / "drivers/input/mouse/mouse.ko").is_file())
        self.assertTrue((allmods / "drivers/tty/serial/serial.ko").is_file())
        self.assertNotIn(" mouse_driver_module", symbols)
        self.assertNotIn(" serial_driver_module", symbols)
        self.assertIn("CONFIG_USB_UHCI_HCD=m", all_config)
        self.assertTrue((allmods / "drivers/usb/uhci/uhci.ko").is_file())
        self.assertNotIn(" uhci_driver_module", symbols)
        filesystems = {
            "EXT_FS_COMMON": "fs/ext_common/ext_common.ko",
            "EXT2_FS": "fs/ext2/ext2.ko", "EXT4_FS": "fs/ext4/ext4.ko",
            "FAT_FS": "fs/fat/vfat.ko", "EXFAT_FS": "fs/exfat/exfat.ko",
            "ISO9660_FS": "fs/iso9660/isofs.ko",
        }
        for symbol, path in filesystems.items():
            self.assertIn("CONFIG_" + symbol + "=m", all_config)
            self.assertTrue((allmods / path).is_file())
            metadata = subprocess.check_output(["readelf", "-p", ".modinfo", str(allmods / path)], text=True)
            self.assertIn("license=Apache-2.0", metadata)
            if symbol in ("EXT2_FS", "EXT4_FS"):
                self.assertIn("depends=ext_common", metadata)
        for function in ("storage_ext4_mount", "fat32_mount", "exfat_mount", "iso_mount"):
            self.assertNotIn(" " + function + "\n", symbols)


if __name__ == "__main__":
    unittest.main()

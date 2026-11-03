#!/usr/bin/env python3
"""Exercise the native USB3 host's production helpers under sanitizers."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HUB_SOURCES = [f"drivers/usb/xhci/{name}.c" for name in
               ("topology", "hub_descriptor", "hub", "hub_ports", "hub_tt", "interrupt")]


def check_capability_header_load(ir):
    """Check volatile access width before machine-register allocation."""
    mapped = re.search(r"^\s*(%[\w.]+) = .*\bcall\b[^\n]*@pci_map_firmware_bar0\(", ir, re.M)
    assert mapped is not None, "missing firmware MMIO mapping"
    offsets = {mapped.group(1): 0}
    loads = []
    for line in ir[mapped.end():].split("\n}", 1)[0].splitlines():
        cast = re.search(r"(%[\w.]+) = bitcast \S+ (%[\w.]+) to ", line)
        if cast and cast.group(2) in offsets:
            offsets[cast.group(1)] = offsets[cast.group(2)]
        address = re.search(
            r"(%[\w.]+) = getelementptr(?: inbounds| nuw| nusw)* i(\d+), "
            r"(?:ptr|i\d+\*) (%[\w.]+), i\d+ (-?\d+)\b", line)
        if address and address.group(3) in offsets:
            offsets[address.group(1)] = (offsets[address.group(3)] +
                int(address.group(2)) // 8 * int(address.group(4)))
        load = re.search(r"load volatile i(\d+), (?:ptr|i\d+\*) (%[\w.]+), align (\d+)", line)
        if load and load.group(2) in offsets:
            offset, width = offsets[load.group(2)], int(load.group(1))
            if offset < 4 and offset + width // 8 > 0:
                loads.append((offset, width, int(load.group(3))))
    assert len(loads) == 1 and loads[0][:2] == (0, 32) and loads[0][2] >= 4, (
        f"capability header must be read once as an aligned dword; got {loads}")


class USBXHCI(unittest.TestCase):
    def test_hub_routes_and_descriptor_bounds(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-hub-layout-") as directory:
            binary = Path(directory) / "hub-layout"
            sources = ["tools/tests/usb_xhci_hub_layout_test.c", "drivers/usb/xhci/topology.c",
                       "drivers/usb/xhci/hub_descriptor.c", "drivers/usb/xhci/hub_tt.c"]
            build = subprocess.run([os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                                    "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                    "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)],
                                   cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_hub_tree_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-hub-tree-") as directory:
            binary = Path(directory) / "hub-tree"
            sources = ["tools/tests/usb_xhci_hub_test.c", "drivers/usb/xhci/devices.c",
                       "drivers/usb/xhci/context.c", "drivers/usb/xhci/classes.c",
                       "drivers/usb/xhci/ring.c", "drivers/usb/xhci/trb.c",
                       "drivers/usb/core/descriptors.c", *HUB_SOURCES]
            build = subprocess.run([os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                                    "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                    "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)],
                                   cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_connection_scoped_port_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-port-lifecycle-") as directory:
            binary = Path(directory) / "ports"
            sources = ["tools/tests/usb_xhci_port_lifecycle_test.c", "drivers/usb/xhci/ports.c",
                       "drivers/usb/xhci/events.c", "drivers/usb/xhci/devices.c",
                       "drivers/usb/xhci/ring.c", "drivers/usb/xhci/completion.c",
                       "drivers/usb/xhci/context.c", "drivers/usb/xhci/classes.c",
                       "drivers/usb/xhci/trb.c", "drivers/usb/core/descriptors.c"]
            sources += HUB_SOURCES
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-Wl,--wrap=xhci_command", "-fsanitize=address,undefined",
                       "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi",
                       *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for case in ("storm", "speed", "warm-speed", "warm-reconnect", "transient",
                         "reset-transient", "undefined-speed", "hub-interface", "interface", "config",
                         "failed-hub-disable", "coalesced-failed-disable",
                         "retry-exhaustion", "reconnect",
                         "disconnect-reconnect", "snapshot-change", "successful-attach-change",
                         "debounce", "reset", "ack-window", "attach"):
                with self.subTest(case=case):
                    run = subprocess.run([str(binary), case], cwd=ROOT, text=True,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
                    self.assertEqual(run.returncode, 0, run.stdout)
                    self.assertIn("USB xHCI connection-scoped port lifecycle passed", run.stdout)

    def test_firmware_mandatory_geometry(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-geometry-") as directory:
            binary = Path(directory) / "geometry"
            sources = ["tools/tests/usb_xhci_geometry_test.c", "drivers/usb/xhci/firmware.c",
                       "drivers/usb/xhci/mmio.c"]
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for case in ("vmware", "qemu", "reserved", "small", "aliases", "invalid"):
                with self.subTest(case=case):
                    run = subprocess.run([str(binary), case], cwd=ROOT, text=True,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
                    self.assertEqual(run.returncode, 0, run.stdout)
                    self.assertIn("USB xHCI firmware geometry case passed", run.stdout)

    def test_event_reserved_padding_and_defined_identity(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-event-padding-") as directory:
            binary = Path(directory) / "events"
            sources = ["tools/tests/usb_xhci_event_padding_test.c", "drivers/usb/xhci/ring.c"]
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for case in ("transfer", "command"):
                with self.subTest(case=case):
                    run = subprocess.run([str(binary), case], cwd=ROOT, text=True,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
                    self.assertEqual(run.returncode, 0, run.stdout)
                    self.assertIn("USB xHCI event reserved padding case passed", run.stdout)

    def test_probe_stack_budget(self):
        # xHCI starts on the 64 KiB interruptible boot stack.  Keep the probe
        # chain's own frame below 8 KiB, leaving room for its caller, callees
        # and interrupts; .su includes saved-register costs.
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-stack-") as directory:
            for mode in ("builtin", "module"):
                with self.subTest(mode=mode):
                    obj = Path(directory) / (mode + ".o")
                    command = ["clang", "--target=x86_64-unknown-none", "-O3", "-std=c11",
                               "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
                               "-mno-red-zone", "-mgeneral-regs-only", "-mcmodel=kernel",
                               "-Wall", "-Wextra", "-fstack-usage", "-DCONFIG_MODULES=1",
                               "-Iinclude", "-Iinclude/uapi"]
                    if mode == "module":
                        command += ["-DMODULE", "-mcmodel=large", "-fno-common",
                                    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
                                    "-Iscripts/module-include"]
                    command += ["-c", "drivers/usb/xhci/controller.c", "-o", str(obj)]
                    build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    usage = obj.with_suffix(".su")
                    self.assertTrue(usage.is_file(), "compiler did not report stack usage")
                    frames = {}
                    kinds = {}
                    for line in usage.read_text().splitlines():
                        fields = line.split("\t")
                        self.assertGreaterEqual(len(fields), 3, line)
                        function = fields[0].rsplit(":", 1)[-1]
                        frames[function] = int(fields[1])
                        kinds[function] = fields[2]
                    self.assertIn("initialize", frames)
                    self.assertEqual(kinds["initialize"], "static")
                    if "probe" in frames:
                        self.assertEqual(kinds["probe"], "static")
                    # At -O3, probe may be inlined into initialize or emitted
                    # separately; in the latter case count both frames.
                    chain = frames["initialize"] + frames.get("probe", 0)
                    self.assertLessEqual(chain, 8192,
                        f"{mode} xHCI initialize/probe stack budget exceeded: {chain} bytes; {frames}")

    def test_superspeedplus_attach(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-ssp-attach-") as directory:
            binary = Path(directory) / "attach"
            sources = ["tools/tests/usb_xhci_ssp_attach_test.c", "drivers/usb/xhci/devices.c",
                       "drivers/usb/xhci/protocol.c", "drivers/usb/xhci/context.c",
                       "drivers/usb/xhci/classes.c", "drivers/usb/xhci/ring.c",
                       "drivers/usb/xhci/trb.c", "drivers/usb/core/descriptors.c"]
            sources += HUB_SOURCES
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI SuperSpeedPlus attach retains raw ID", run.stdout)

    def test_dma_activation_order_and_failure_ownership(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-start-") as directory:
            binary = Path(directory) / "start"
            sources = ["tools/tests/usb_xhci_start_test.c", "drivers/usb/xhci/startup.c",
                       "drivers/usb/xhci/lifecycle.c"]
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI owned DMA starts with BME and dequeue before ERSTBA", run.stdout)

    def test_firmware_capabilities_do_not_alias_live_registers(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-firmware-overlap-") as directory:
            binary = Path(directory) / "firmware-overlap"
            sources = ["tools/tests/usb_xhci_firmware_overlap_test.c", "drivers/usb/xhci/firmware.c",
                       "drivers/usb/xhci/mmio.c"]
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI firmware capability records avoid live operational registers", run.stdout)

    def test_probe_reads_capability_header_as_one_dword(self):
        # The QEMU xHCI MMIO register accepts the architected dword read;
        # a byte/halfword read of the same header can return a different value.
        # LLVM retains access widths and SSA pointer identities without tying
        # this hardware contract to a particular machine-register allocation.
        production = (ROOT / "drivers/usb/xhci/controller.c").read_text()
        original = "uint32_t header=*(volatile uint32_t *)initial;"
        self.assertEqual(production.count(original), 1)
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-cap-width-") as directory:
            for width, extra_read in ((32, False), (8, False), (16, False), (32, True)):
                with self.subTest(width=width, extra_read=extra_read):
                    source = Path(directory) / "controller.c"
                    ir = Path(directory) / "controller.ll"
                    text = production
                    if width != 32:
                        # Negative controls still read the version, but use an
                        # invalid narrow access for the first capability bytes.
                        text = text.replace(original,
                            f"uint32_t header=*(volatile uint{width}_t *)initial | "
                            "((uint32_t)*(volatile uint16_t *)(initial+2)<<16);")
                    elif extra_read:
                        text = text.replace(original, original +
                            "\n    (void)*(volatile uint16_t *)(initial+2);")
                    source.write_text(text)
                    command = ["clang", "-std=gnu11", "-O1", "-fno-inline",
                               "-Iinclude", "-Iinclude/uapi", "-Idrivers/usb/xhci",
                               "-S", "-emit-llvm", str(source), "-o", str(ir)]
                    build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    if width == 32 and not extra_read:
                        check_capability_header_load(ir.read_text())
                    else:
                        with self.assertRaisesRegex(AssertionError, "aligned dword"):
                            check_capability_header_load(ir.read_text())

    def test_controller_failure_revokes_classes(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-failure-") as directory:
            binary = Path(directory) / "failure"
            sources = ["tools/tests/usb_xhci_failure_test.c", "drivers/usb/xhci/ports.c",
                       "drivers/usb/xhci/devices.c", "drivers/usb/xhci/events.c",
                       "drivers/usb/xhci/ring.c", "drivers/usb/xhci/completion.c",
                       "drivers/usb/storage/bulk_only.c", "drivers/usb/core/registry.c",
                       "drivers/usb/core/hid.c", "block/device/registry.c"]
            sources += HUB_SOURCES + ["drivers/usb/xhci/trb.c"]
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-DCONFIG_MODULES=1", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-Wl,--wrap=xhci_device_attach", "-Wl,--wrap=xhci_device_attach_child", "-Wl,--wrap=xhci_hid_arm",
                       "-Wl,--wrap=xhci_hid_event", "-fsanitize=address,undefined",
                       "-fno-sanitize-recover=all", "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI controller failure releases HID and revokes retained disks while keeping DMA", run.stdout)

    def test_retained_disk_collection(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-retire-") as directory:
            binary = Path(directory) / "retire"
            sources = ["tools/tests/usb_xhci_retire_test.c", "drivers/usb/xhci/ports.c",
                       "drivers/usb/xhci/devices.c", "drivers/usb/storage/bulk_only.c",
                       "drivers/usb/core/registry.c", "block/device/registry.c"]
            sources += HUB_SOURCES + ["drivers/usb/xhci/ring.c", "drivers/usb/xhci/trb.c"]
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-DCONFIG_MODULES=1", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-Wl,--wrap=xhci_device_attach", "-Wl,--wrap=xhci_device_attach_child", "-Wl,--wrap=xhci_hid_arm",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI retained disk retires after last close without a new port event", run.stdout)

    def test_port_change_acknowledgement(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-ports-") as directory:
            binary = Path(directory) / "ports"
            sources = ["tools/tests/usb_xhci_ports_test.c", "drivers/usb/xhci/ports.c"]
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI port change acknowledgement and fatal status tests passed", run.stdout)

    def test_mmio_address_width(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-mmio-") as directory:
            binary = Path(directory) / "mmio"
            sources = ["tools/tests/usb_xhci_mmio_test.c"]
            source = ROOT / "drivers/usb/xhci/mmio.c"
            if source.exists():
                sources.append(str(source))
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI MMIO 32/64-bit address tests passed", run.stdout)

    def test_firmware_ownership_handshake(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-handoff-") as directory:
            binary = Path(directory) / "handoff"
            sources = ["tools/tests/usb_xhci_handoff_test.c", "drivers/usb/xhci/firmware.c"]
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI firmware ownership handshake tests passed", run.stdout)

    def test_idle_halt_reset_and_ownership(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-recovery-") as directory:
            binary = Path(directory) / "recovery"
            sources = ["tools/tests/usb_xhci_recovery_test.c", "drivers/usb/xhci/transfer.c",
                       "drivers/usb/xhci/hub_tt.c",
                       "drivers/usb/xhci/trb.c", "drivers/usb/xhci/ring.c",
                       "drivers/usb/xhci/context.c", "drivers/usb/xhci/completion.c"]
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI idle halt reset and uncertain command ownership tests passed", run.stdout)

    def test_device_class_validation(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-class-") as directory:
            binary = Path(directory) / "class"
            sources = ["tools/tests/usb_xhci_class_test.c"]
            source = ROOT / "drivers/usb/xhci/classes.c"
            if source.exists():
                sources.append(str(source))
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI device class and EP0 validation tests passed", run.stdout)

    def test_controller_stop_ownership(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-stop-") as directory:
            binary = Path(directory) / "stop"
            sources = ["tools/tests/usb_xhci_lifecycle_test.c"]
            source = ROOT / "drivers/usb/xhci/lifecycle.c"
            if source.exists():
                sources.append(str(source))
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI stop and DMA ownership tests passed", run.stdout)

    def test_transfer_trbs(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-transfer-") as directory:
            binary = Path(directory) / "transfer"
            sources = ["tools/tests/usb_xhci_transfer_test.c"]
            source = ROOT / "drivers/usb/xhci/trb.c"
            if source.exists():
                sources.append(str(source))
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI control and bulk TRB tests passed", run.stdout)

    def test_control_and_bulk_completions(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-completion-") as directory:
            binary = Path(directory) / "completion"
            sources = ["tools/tests/usb_xhci_completion_test.c", "drivers/usb/xhci/ring.c"]
            source = ROOT / "drivers/usb/xhci/completion.c"
            if source.exists():
                sources.append(str(source))
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI control and bulk completion tests passed", run.stdout)

    def test_firmware_capability_bounds(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-firmware-") as directory:
            binary = Path(directory) / "firmware"
            sources = ["tools/tests/usb_xhci_firmware_test.c"]
            source = ROOT / "drivers/usb/xhci/firmware.c"
            if source.exists():
                sources.append(str(source))
                sources.append("drivers/usb/xhci/mmio.c")
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI firmware capability bounds tests passed", run.stdout)

    def test_supported_protocols(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-protocol-") as directory:
            binary = Path(directory) / "protocol"
            sources = ["tools/tests/usb_xhci_protocol_test.c"]
            parser = ROOT / "drivers/usb/xhci/protocol.c"
            if parser.exists():
                sources.append(str(parser))
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI supported protocol tests passed", run.stdout)

    def test_contexts_and_scratchpads(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-context-") as directory:
            binary = Path(directory) / "context"
            sources = ["tools/tests/usb_xhci_context_test.c"]
            context = ROOT / "drivers/usb/xhci/context.c"
            if context.exists():
                sources.append(str(context))
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI context and scratchpad tests passed", run.stdout)

    def test_ring_ownership_and_events(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-ring-") as directory:
            binary = Path(directory) / "ring"
            sources = ["tools/tests/usb_xhci_ring_test.c"]
            ring = ROOT / "drivers/usb/xhci/ring.c"
            if ring.exists():
                sources.append(str(ring))
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", *sources, "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB xHCI ring ownership and event tests passed", run.stdout)

    def test_superspeed_descriptors(self):
        with tempfile.TemporaryDirectory(prefix="reliefnt-xhci-descriptor-") as directory:
            binary = Path(directory) / "descriptor"
            parser = ROOT / "drivers/usb/core/descriptors.c"
            if not parser.exists():
                parser = ROOT / "drivers/usb/ehci/descriptors.c"
            command = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
                       "-Wall", "-Wextra", "-Werror", "-fno-pie", "-no-pie",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                       "-Iinclude", "-Iinclude/uapi", "tools/tests/usb_xhci_descriptor_test.c",
                       str(parser), "drivers/usb/xhci/classes.c", "-o", str(binary)]
            build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn("USB SuperSpeed descriptor tests passed", run.stdout)


if __name__ == "__main__":
    unittest.main()

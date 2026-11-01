#!/usr/bin/env python3
"""Host-side contract checks for the native x86-64 Linux syscall ABI."""

from pathlib import Path, PurePosixPath
import json
import re


from kernel_test_sources import kernel_sources, module_text

ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def macro(text: str, name: str) -> int:
    match = re.search(rf"^#define {re.escape(name)}\s+(0x[0-9a-fA-F]+|\d+)", text, re.M)
    assert match, f"missing {name}"
    return int(match.group(1), 0)


def test_linux_numbers_and_flags() -> None:
    syscall = read("include/uapi/linux/syscall.h")
    fcntl = read("include/uapi/linux/fcntl.h")
    assert macro(syscall, "__NR_pause") == 34
    assert macro(syscall, "__NR_arch_prctl") == 158
    assert macro(syscall, "__NR_set_tid_address") == 218
    assert macro(syscall, "__NR_exit_group") == 231
    assert macro(syscall, "__NR_gettid") == 186
    assert macro(fcntl, "LINUX_O_NONBLOCK") == 0x800
    assert macro(fcntl, "LINUX_F_DUPFD_CLOEXEC") == 1030


def test_native_syscall_entry_and_stack_protocol() -> None:
    boot_asm = read("arch/x86_64/boot/boot.S")
    gdt = read("arch/x86_64/cpu/gdt.c")
    userland = read("kernel/exec/userland.c")
    assert "x86_64_syscall_entry" in boot_asm
    assert "X86_IA32_EFER" in gdt and "read_msr(X86_IA32_EFER) | 1ULL" in gdt
    assert "AT_PHDR" in userland and "AT_RANDOM" in userland


def test_contract_fixes_are_present() -> None:
    syscall_h = read("include/reliefnt/syscall.h")
    process = read("kernel/syscall/process/process.c")
    syscall = module_text("syscall_dispatch")
    ipc = read("kernel/syscall/ipc/ipc.c")
    assert "LINUX_SYS_PAUSE" in syscall_h
    assert "LINUX_SYS_NICE __NR_nice" not in syscall_h
    assert "SIG_BLOCK=0" in process or "SIG_BLOCK" in process
    assert "case LINUX_SYS_PAUSE" in syscall
    assert "flags & ~(uint32_t)(RELIEFOS_O_NONBLOCK | RELIEFOS_O_CLOEXEC)" in ipc
    assert "return -RELIEFOS_EBADF" in syscall


def test_audio_export_closure() -> None:
    # Host /usr/include can conceal a missing dependency. Both root and kernel
    # export lists must publish the complete public sound include closure.
    needed = {"include/uapi/sound/asound.h", "include/uapi/sound/tlv.h",
              "include/uapi/linux/soundcard.h", "include/uapi/linux/patchkey.h",
              "include/uapi/linux/time.h", "include/uapi/linux/types.h",
              "include/uapi/linux/ioctl.h"}
    for path in ("configs/header-export.list",):
        exported = {line.strip() for line in read(path).splitlines() if not line.startswith("#")}
        assert needed <= exported, f"{path}: missing sound closure {needed - exported}"


def check_audio_validation(validation) -> None:
    """Validate bounded evidence declarations; this does not rerun guest tests."""
    for category in ("source_host_fixture", "guest_binary_build", "guest_runtime",
                     "upstream_alsa", "physical_hardware"):
        assert isinstance(validation.get(category), bool), category

    def evidence(field):
        text = validation.get(field)
        assert isinstance(text, str) and text.strip(), f"missing {field}"
        for item in text.split(";"):
            path = PurePosixPath(item.strip())
            assert path.parts and path.parts[0] == "out" and ".." not in path.parts, field
            assert path.suffix == ".json", field

    def scope(field):
        text = validation.get(field)
        assert isinstance(text, str) and text.strip(), f"missing {field}"

    if validation["guest_runtime"]:
        assert validation["source_host_fixture"] and validation["guest_binary_build"], "guest prerequisites"
        scope("guest_probe_modes_verified")
        evidence("guest_probe_evidence")
    if validation["upstream_alsa"]:
        assert validation["guest_runtime"], "upstream ALSA requires guest validation"
        scope("upstream_alsa_scope")
    if validation["physical_hardware"]:
        assert validation["guest_runtime"], "hardware requires runtime validation"
        scope("physical_hardware_scope")
        evidence("physical_hardware_evidence")


def test_audio_support_manifest() -> None:
    manifest = json.loads(read("configs/audio-abi-support.json"))
    assert manifest["uapi"] == "Linux v6.14 native x86-64"
    expected = set(re.findall(r"^#define\s+(SNDRV_(?:PCM|CTL)_IOCTL_\w+)",
                              read("include/uapi/sound/asound.h"), re.M))
    expected |= set(re.findall(r"^#define\s+((?:SNDCTL_DSP|SOUND_PCM_(?:READ|WRITE))_\w+)",
                              read("include/uapi/linux/soundcard.h"), re.M))
    assert expected == set(manifest["ioctls"]), f"manifest ioctl set differs: {expected ^ set(manifest['ioctls'])}"
    for name, entry in manifest["ioctls"].items():
        assert isinstance(entry["supported"], bool), name
        for field in ("access", "state", "errno", "mmap", "evidence"):
            assert entry[field], (name, field)
    for path in re.findall(r"tools/(?:test_\w+\.py|tests/\w+\.c)", json.dumps(manifest)):
        assert (ROOT / path).is_file(), f"manifest evidence file missing: {path}"
    check_audio_validation(manifest["validation"])
    assert manifest["ioctls"]["SNDRV_PCM_IOCTL_WRITEN_FRAMES"]["supported"] is False
    assert manifest["ioctls"]["SNDRV_CTL_IOCTL_RAWMIDI_INFO"]["supported"] is False


def test_audio_validation_rejects_unsubstantiated_claims() -> None:
    valid = {"source_host_fixture": True, "guest_binary_build": True,
             "guest_runtime": True, "upstream_alsa": True, "physical_hardware": False,
             "guest_probe_modes_verified": "Fixture playback only",
             "guest_probe_evidence": "out/audio-fixture/result.json",
             "upstream_alsa_scope": "Fixture playback only"}
    check_audio_validation(valid)
    for field, value in (("guest_runtime", "true"), ("source_host_fixture", False),
                         ("guest_binary_build", False), ("guest_probe_evidence", ""),
                         ("guest_probe_evidence", "out/../untrusted/result.json"),
                         ("guest_probe_modes_verified", ""), ("upstream_alsa_scope", ""),
                         ("physical_hardware", True)):
        invalid = dict(valid)
        invalid[field] = value
        try:
            check_audio_validation(invalid)
        except AssertionError:
            continue
        raise AssertionError(f"unsupported audio validation claim accepted: {field}={value!r}")


if __name__ == "__main__":
    tests = [test_linux_numbers_and_flags,
             test_native_syscall_entry_and_stack_protocol,
             test_contract_fixes_are_present, test_audio_export_closure,
             test_audio_support_manifest, test_audio_validation_rejects_unsubstantiated_claims]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")

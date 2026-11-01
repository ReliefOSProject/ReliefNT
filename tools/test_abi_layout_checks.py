#!/usr/bin/env python3
"""Check real wire layouts and ensure the ABI gate rejects incompatible edits."""
import copy
import unittest

from test_abi_layout import (
    collect, compare_abi, discover_structs, expected_baseline, normalize_decl, wire_header_paths,
)


class ABIChanges(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.expected = expected_baseline()
        cls.current = collect(discover_structs(wire_header_paths()))

    def test_current_wire_contract(self):
        self.assertEqual(compare_abi(self.expected, self.current), [])

    def test_incompatible_driver_edits_are_rejected(self):
        for case in ("version", "prefix-offset", "prefix-type", "tail-offset",
                     "tail-type", "size", "extra-service"):
            with self.subTest(case=case):
                current = copy.deepcopy(self.current)
                api = current["layout"]["reliefos_driver_kernel_api"]
                if case == "version":
                    current["consts"]["RELIEFOS_DRIVER_ABI_VERSION"] = 1
                elif case == "prefix-offset":
                    api["members"][2][0] += 8
                elif case == "prefix-type":
                    api["members"][2][1] = "uint64_t (*)(uint16_t) inb"
                elif case == "tail-offset":
                    api["members"][-1][0] += 8
                elif case == "tail-type":
                    api["members"][-1][1] = "void *(*)(uint32_t) phys_to_virt"
                elif case == "size":
                    api["size"] += 8
                else:
                    api["members"].append([304, "void (*)(void) extra_service"])
                self.assertTrue(compare_abi(self.expected, current), case)

    def test_unchanged_anonymous_type_across_clang_versions(self):
        prefix = "struct reliefos_net_control::"
        suffix = "(unnamed at /checkout/include/uapi/reliefos/net_control.h:23:9) connections"
        self.assertEqual(normalize_decl(prefix + "(unnamed union)::" + suffix),
                         normalize_decl(prefix + suffix))

    def test_net_control_changes_are_still_rejected(self):
        for key in ("offset", "type", "size", "align"):
            with self.subTest(key=key):
                current = copy.deepcopy(self.current)
                record = current["layout"]["reliefos_net_control"]
                if key == "offset":
                    record["members"][0][0] += 4
                elif key == "type":
                    record["members"][0][1] = "uint64_t version"
                else:
                    record[key] += 4
                self.assertTrue(compare_abi(self.expected, current), key)


if __name__ == "__main__":
    unittest.main()

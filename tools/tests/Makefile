# SPDX-License-Identifier: Apache-2.0
# Test orchestration only; the production build does not invoke Python.
.PHONY: test-host test-host-smoke test-smp-time test-page-cache-growth \
 test-user-return test-syscall-io test-vdso-host test-ownership

RELIEFNT_HOST_TEST_RUN = $(RELIEFNT_TEST_PYTHON) $(RELIEFNT_SRC)/tools/test_host.py \
 --output '$(O)/host-regressions'

test-host:
	$(Q)$(RELIEFNT_HOST_TEST_RUN)

test-host-smoke:
	$(Q)$(RELIEFNT_HOST_TEST_RUN) --suite boot_policy --suite brand_identity --suite gpu --suite drm --suite svga \
	 --suite loadavg --suite input --suite input_routing --suite uapi \
	 --suite acpi_pm_timer --suite apic_timer --suite cpu_accounting \
	 --suite vdso_mapping --suite vdso_time

test-smp-time:
	$(Q)$(RELIEFNT_HOST_TEST_RUN) --suite acpi_pm_timer --suite apic_timer --suite cpu_accounting

test-page-cache-growth:
	$(Q)$(RELIEFNT_HOST_TEST_RUN) --suite page_cache_growth

test-user-return:
	$(Q)$(RELIEFNT_HOST_TEST_RUN) --suite user_return_gate --suite execution_lock --suite sched_resume

test-syscall-io:
	$(Q)$(RELIEFNT_HOST_TEST_RUN) --suite syscall_io

test-vdso-host:
	$(Q)$(RELIEFNT_HOST_TEST_RUN) --suite vdso_mapping --suite vdso_time

test-ownership:
	$(Q)$(RELIEFNT_TEST_PYTHON) $(RELIEFNT_SRC)/tools/test_test_ownership.py

.PHONY: test-version
test-version:
	$(Q)sh $(RELIEFNT_SRC)/tools/tests/test-version.sh

.PHONY: test-legacy
test-legacy:
	$(Q)$(RELIEFNT_HOST_TEST_RUN) --suite uapi --suite regular_file_io \
	 --suite storage_metadata --suite storage_rename --suite storage_mkdir_mount \
	 --suite storage_sync --suite record_locks

.PHONY: test-ext4-reference
test-ext4-reference:
	$(Q)test -n '$(LINUX_REFERENCE)' || { printf '%s\n' 'set LINUX_REFERENCE to an existing Linux reference source tree'; exit 1; }
	$(Q)$(RELIEFNT_TEST_PYTHON) $(RELIEFNT_SRC)/tools/tests/ext4_feature_matrix.py \
	 --check-schema --linux-root '$(abspath $(LINUX_REFERENCE))'

# ReliefNT standalone kernel build entry point (phase 2 of the kernel/userland
# separation).
#
# GNU Make owns the dependency graph, the parallel schedule and the incremental
# decisions. C helpers under tools/host/ perform data transforms only; short
# scripts under tools/build/ drive third-party builds. Nothing here shells out to
# a second scheduler, and no production path runs Python, Meson or Ninja (the
# Python regression tools under tools/test_*.py run only from `make test`).
#
# Derived from the parent ReliefOS build entry point. The product surface here
# is exactly the kernel side: kernel.sys, kernel.debug, loader.elf, the five
# .drv drivers and kerneldebug.sys. Nothing in this tree reads parent or
# product configuration.

# --- GNU Make version ------------------------------------------------------
# Grouped targets ('&:') and the $(file) function both need 4.3.
reliefos_make_min := $(shell printf '4.3\n$(MAKE_VERSION)\n' | LC_ALL=C sort -V | head -n1)
ifeq ($(reliefos_make_min),4.3)
else
$(error GNU Make >= 4.3 is required, this is $(MAKE_VERSION))
endif

RELIEFOS_SRC := $(patsubst %/,%,$(dir $(realpath $(firstword $(MAKEFILE_LIST)))))

# --- user-facing variables --------------------------------------------------
# ARCH, PROFILE and O may come from the command line or from these defaults
# only. An inherited environment value is ignored on purpose: an unrelated shell
# setting must not silently change what gets built (plan section 6.2).
ifeq ($(origin ARCH),undefined)
ARCH := x86_64
endif
ifeq ($(origin PROFILE),undefined)
PROFILE := release
endif
ifeq ($(origin O),undefined)
O := $(RELIEFOS_SRC)/out/$(ARCH)/$(PROFILE)
endif

V ?= 0
# Appended verbatim to configs/build-version's numeric release, as in Linux.
# Set EXTRAVERSION = -perf here or pass make EXTRAVERSION=-perf.
EXTRAVERSION =
LOCALVERSION ?=
SOURCE_DATE_EPOCH ?= $(shell git -C $(RELIEFOS_SRC) show -s --format=%ct HEAD 2>/dev/null)
TOOLCHAIN ?= $(RELIEFOS_SRC)/configs/toolchains/llvm-x86_64.mk

O := $(patsubst %/,%,$(O))

# --- input validation -------------------------------------------------------
# The supported character set is enumerated rather than promising arbitrary
# paths: Make word splitting, shell quoting and the generated manifests all break
# on the rejected set, so failing here is far cheaper than failing mid-build.
RELIEFOS_ALLOWED_CHARS := a b c d e f g h i j k l m n o p q r s t u v w x y z \
	A B C D E F G H I J K L M N O P Q R S T U V W X Y Z \
	0 1 2 3 4 5 6 7 8 9 . _ / -

# $(call strip_allowed,text,chars): keep only characters outside the allow-list.
strip_allowed = $(if $(2),$(call strip_allowed,$(subst $(firstword $(2)),,$(1)),$(wordlist 2,9999,$(2))),$(1))

reliefos_suffix_residual := $(call strip_allowed,$(EXTRAVERSION)$(LOCALVERSION),$(filter-out /,$(RELIEFOS_ALLOWED_CHARS)) +)
ifneq ($(reliefos_suffix_residual),)
$(error EXTRAVERSION and LOCALVERSION accept only A-Z a-z 0-9 . _ + -)
endif

ifeq ($(O),)
$(error O= must not be empty; it names this build's output directory)
endif
ifneq ($(words $(O)),1)
$(error O='$(O)' is unsupported: whitespace and newlines are not accepted in an output path)
endif
reliefos_O_residual := $(call strip_allowed,$(O),$(RELIEFOS_ALLOWED_CHARS))
ifneq ($(reliefos_O_residual),)
$(error O='$(O)' is unsupported: offending characters are [$(reliefos_O_residual)]; accepted are A-Z a-z 0-9 . _ / -)
endif
# Compare absolute paths: `O=.` and `O=<src>` are the same refusal. O has already
# been restricted to a safe character set above, so quoting here cannot be escaped.
reliefos_O_absolute := $(shell realpath -m -- '$(O)' 2>/dev/null || printf '%s' '$(O)')
ifeq ($(reliefos_O_absolute),/)
$(error refusing / as the output directory)
endif
ifeq ($(reliefos_O_absolute),$(RELIEFOS_SRC))
$(error refusing the source root as the output directory (O='$(O)'))
endif
ifneq ($(filter $(ARCH),x86_64),)
else
$(error unsupported ARCH '$(ARCH)'; this build supports ARCH=x86_64)
endif
ifneq ($(filter $(PROFILE),debug release),)
else
$(error unsupported PROFILE '$(PROFILE)'; use PROFILE=debug or PROFILE=release)
endif

# --- output layout ----------------------------------------------------------
O_HOST      := $(O)/host
O_OBJ       := $(O)/obj
O_GENERATED := $(O)/generated
O_INCLUDE   := $(O)/include
O_CONFIG    := $(O)/config
O_LOGS      := $(O)/logs
O_META      := $(O)/meta

# Shared download cache: deliberately outside O because it is profile
# independent and `distclean` must not throw it away. It is a real directory in
# this repository (never a symlink to another checkout).
RELIEFOS_CACHE := $(RELIEFOS_SRC)/cache/downloads

# Written when an output tree is created and re-checked before anything is
# deleted, so `clean` can never operate on a directory it does not own.
RELIEFOS_O_MARKER := $(O)/.reliefos-out

# --- same-output-directory mutual exclusion ---------------------------------
# Two top-level makes sharing one O would race on objects, generated headers and
# signature files, so one of them has to be refused outright (plan section 6.3).
# Different O directories are independent and may build concurrently.
#
# The owner is the make process that acquired the lock, and the token names the
# output directory it owns: RELIEFOS_BUILD_OWNER is "<pid>.<start ticks>:<absolute O>".
# A nested make for that same directory inherits it, so recursive build
# invocations do not refuse their own outer build. A nested make for a different
# directory acquires its own lock, so a test suite that spawns builds still gets
# real exclusion for the trees it creates.
#
# Three cases skip acquisition: dry runs and `make -q` (they promise no output,
# and refusing them would make `make -n` depend on unrelated builds), and goals
# that write nothing at all -- a bare `make` and `make help` must keep working
# while a build is running elsewhere, and must not create the output tree.
reliefos_read_only_goals := help
reliefos_lock_not_needed :=
ifeq ($(MAKECMDGOALS),)
reliefos_lock_not_needed := 1
else ifeq ($(words $(filter $(reliefos_read_only_goals),$(MAKECMDGOALS))),$(words $(MAKECMDGOALS)))
reliefos_lock_not_needed := 1
endif
reliefos_lock_dir := $(O)/.build-lock
reliefos_lock_skip := \
	$(if $(findstring n,$(firstword -$(MAKEFLAGS))),1)\
	$(if $(findstring q,$(firstword -$(MAKEFLAGS))),1)\
	$(reliefos_lock_not_needed)
ifeq ($(strip $(reliefos_lock_skip)),)
RELIEFOS_BUILD_OWNER := $(shell sh $(RELIEFOS_SRC)/scripts/build-lock.sh acquire \
	$(reliefos_lock_dir) $(reliefos_O_absolute))
ifeq ($(RELIEFOS_BUILD_OWNER),)
$(error refusing to build: '$(O)' is already being built by another make)
endif
export RELIEFOS_BUILD_OWNER
endif

# --- fragment includes ------------------------------------------------------
# Kernel-side machinery only. The parent's userland/sdk/images fragments (and
# the parse-time components.mk coupling in mk/userland.mk) are deliberately
# absent from this tree.
include $(RELIEFOS_SRC)/mk/logging.mk
include $(RELIEFOS_SRC)/mk/host.mk
include $(RELIEFOS_SRC)/mk/toolchain.mk
include $(RELIEFOS_SRC)/mk/config.mk
include $(RELIEFOS_SRC)/mk/kernel.mk
include $(RELIEFOS_SRC)/mk/headers.mk
include $(RELIEFOS_SRC)/mk/boot.mk
include $(RELIEFOS_SRC)/mk/resources.mk

# --- public goals -----------------------------------------------------------
.DEFAULT_GOAL := help
# Source inventories are inputs, never implicit host executable targets.
.SUFFIXES:

.PHONY: help all kernel loader drivers boot tools fetch defconfig olddefconfig \
	menuconfig headers_install install config-sync build-info \
	test test-tools test-abi test-header-export test-uapi-compat clean distclean

help:
	@printf '%s\n' \
	  'ReliefNT kernel build (standalone kernel checkout)' \
	  '' \
	  '  all               kernel.sys, kernel.debug, loader.elf, five .drv, kerneldebug.sys' \
	  '  kernel            kernel.sys + kernel.debug' \
	  '  loader            loader.elf (waits for kernel.sys: loader integrity chain)' \
	  '  drivers           mouse.drv serial.drv e1000.drv ac97.drv es1371.drv hda.drv + kerneldebug.sys' \
	  '  headers_install   export the UAPI whitelist to $(O)/kernel-export/include' \
	  '  install           copy products + manifest.txt to $(DESTDIR)' \
	  '  test              host-tool tests + ABI layout + header export + UAPI compat' \
	  '  tools             the host C helpers' \
	  '  fetch             download the locked dependencies into cache/downloads' \
	  '  defconfig / olddefconfig / menuconfig' \
	  '  clean / distclean (distclean also removes the configuration)' \
	  '' \
	  '  variables: O= ARCH= PROFILE=release|debug TOOLCHAIN= SOURCE_DATE_EPOCH= V=1 HOSTCC='

tools: $(RELIEFOS_HOST_TOOLS)

kernel: $(RELIEFOS_KERNEL_SYS) $(RELIEFOS_KERNEL_DEBUG)

# `all` is exactly the six kernel products. There is no userland, no image and
# no package goal in this repository.
all: kernel loader drivers

# Populate the shared download cache from the locked URLs. A normal build never
# downloads: it verifies the cache and stops with this command when bytes are
# missing.
fetch: $(RELIEFOS_DEPS_TOOL)
	$(Q)sh $(RELIEFOS_SRC)/tools/build/fetch.sh --deps $(RELIEFOS_DEPS_TOOL) \
		--lock $(RELIEFOS_SRC)/configs/dependencies.lock.json --cache $(RELIEFOS_CACHE) \
		--only unifont

defconfig olddefconfig menuconfig: $(RELIEFOS_O_MARKER) $(KCONFIG_CONF) $(KCONFIG_MCONF) | $(O_CONFIG)
	$(Q)sh $(RELIEFOS_SRC)/tools/build/kconfig-frontends.sh run \
		--conf $(abspath $(KCONFIG_CONF)) --mconf $(abspath $(KCONFIG_MCONF)) \
		--kconfig $(KCONFIG_ROOT) --config $(abspath $(RELIEFOS_CONFIG_FILE)) \
		--seed $(KCONFIG_SEED) --mode $@

config-sync: $(AUTOCONF_H) $(RELIEFOS_AUTOCONF_MK)

build-info: $(BUILD_INFO_HEADER)

# --- install ---------------------------------------------------------------
# Products plus manifest.txt. The manifest records what was installed and how
# it was built so an installed kernel can be traced back to its inputs: format
# version, arch, kernel git identity (or "no-git"), dirty flag, toolchain
# identity, config and UAPI export digests, the boot handoff version and a
# sha256 per artifact. sha256sum only; no interpreter in the production chain.
DESTDIR ?= $(O)/kernel-install
INSTALL_PRODUCTS := $(RELIEFOS_KERNEL_SYS) $(RELIEFOS_KERNEL_DEBUG) $(KERNELDEBUG_SYS) \
	$(LOADER_ELF) $(DRIVER_OUTPUTS)

.PHONY: install
install: $(INSTALL_PRODUCTS) $(HEADER_EXPORT_MANIFEST)
	$(call RELIEFOS_LOG,INSTALL,$(DESTDIR))
	$(Q)set -eu; \
	dest='$(DESTDIR)'; \
	mkdir -p "$$dest"; \
	cp $(INSTALL_PRODUCTS) "$$dest"/; \
	{ \
	  printf 'format_version: 1\n'; \
	  printf 'arch: %s\n' '$(ARCH)'; \
	  printf 'profile: %s\n' '$(PROFILE)'; \
	  if git -C '$(RELIEFOS_SRC)' rev-parse --short HEAD >/dev/null 2>&1; then \
	    printf 'kernel_git: %s\n' "$$(git -C '$(RELIEFOS_SRC)' rev-parse --short HEAD)"; \
	    if [ -n "$$(git -C '$(RELIEFOS_SRC)' status --porcelain 2>/dev/null)" ]; then \
	      printf 'kernel_git_dirty: yes\n'; \
	    else \
	      printf 'kernel_git_dirty: no\n'; \
	    fi; \
	  else \
	    printf 'kernel_git: no-git\n'; \
	    printf 'kernel_git_dirty: unknown\n'; \
	  fi; \
	  printf 'toolchain: %s\n' "$$($(TARGET_CC) --version 2>/dev/null | head -n1)"; \
	  printf 'config_sha256: %s\n' "$$(sha256sum '$(RELIEFOS_CONFIG_FILE)' | cut -d' ' -f1)"; \
	  printf 'uapi_sha256: %s\n' "$$(sha256sum '$(HEADER_EXPORT_MANIFEST)' | cut -d' ' -f1)"; \
	  printf 'handoff_version: %s\n' "$$(sed -n 's/^[[:space:]]*#define[[:space:]]\{1,\}RELIEFOS_BOOT_HANDOFF_VERSION[[:space:]]\{1,\}\([0-9][0-9]*\).*/\1/p' '$(RELIEFOS_SRC)/include/reliefos/boot_handoff.h' | head -n1)"; \
	  printf 'artifacts:\n'; \
	  (cd "$$dest" && sha256sum $(notdir $(INSTALL_PRODUCTS)) | sed 's/^/  /'); \
	} > "$$dest/manifest.txt.tmp"; \
	mv "$$dest/manifest.txt.tmp" "$$dest/manifest.txt"

# --- tests -----------------------------------------------------------------
# The two Python regression tools are boundary tests, not production chain.
# The C host tests cover the shared host primitives and the lock-file reader.
# LEONOS_TEST_PYTHON is the pre-rename input spelling; the new name wins.
RELIEFOS_TEST_PYTHON ?= $(or $(LEONOS_TEST_PYTHON),python3)
RELIEFOS_HOST_TEST_BINS := $(O_HOST)/tests/test_common $(O_HOST)/tests/test_json
RELIEFOS_HOST_TEST_SANITISED := $(O_HOST)/tests-sanitised/test_common \
	$(O_HOST)/tests-sanitised/test_json

$(O_HOST)/obj/tests/host/%.c.o: $(RELIEFOS_SRC)/tests/host/%.c $(O_META)/host-cc.sig
	$(Q)mkdir -p $(dir $@)
	$(call RELIEFOS_LOG,HOSTCC,$<)
	$(Q)$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) -I$(RELIEFOS_SRC)/tests/host \
	    $(RELIEFOS_HOST_INCLUDES) $(HOST_CFLAGS) -MMD -MF $@.d -c $< -o $@

$(O_HOST)/obj/tests-sanitised/host/%.c.o: $(RELIEFOS_SRC)/tests/host/%.c $(O_META)/host-cc-sanitised.sig
	$(Q)mkdir -p $(dir $@)
	$(call RELIEFOS_LOG,HOSTCC,$<)
	$(Q)$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) $(RELIEFOS_SANITISE) \
	    -I$(RELIEFOS_SRC)/tests/host $(RELIEFOS_HOST_INCLUDES) -g -O1 \
	    -MMD -MF $@.d -c $< -o $@

$(O_HOST)/obj/tests-sanitised/tools/host/%.c.o: $(RELIEFOS_SRC)/tools/host/%.c \
	$(O_META)/host-cc-sanitised.sig
	$(Q)mkdir -p $(dir $@)
	$(call RELIEFOS_LOG,HOSTCC,$<)
	$(Q)$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) $(RELIEFOS_SANITISE) -g -O1 \
	    $(RELIEFOS_HOST_INCLUDES) -MMD -MF $@.d -c $< -o $@

$(O_HOST)/tests/test_common: $(O_HOST)/obj/tests/host/test_common.c.o $(RELIEFOS_HOST_COMMON_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@

$(O_HOST)/tests/test_json: $(O_HOST)/obj/tests/host/test_json.c.o $(RELIEFOS_JSON_OBJ) \
	$(RELIEFOS_HOST_COMMON_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@

RELIEFOS_HOST_SANITISED_OBJS := $(patsubst $(O_HOST)/obj/tools/host/%.c.o, \
	$(O_HOST)/obj/tests-sanitised/tools/host/%.c.o,$(RELIEFOS_HOST_COMMON_OBJS))

$(O_HOST)/tests-sanitised/test_common: $(O_HOST)/obj/tests-sanitised/host/test_common.c.o \
	$(RELIEFOS_HOST_SANITISED_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOSTCC) $(RELIEFOS_SANITISE) -g -O1 $^ -o $@

$(O_HOST)/tests-sanitised/test_json: $(O_HOST)/obj/tests-sanitised/host/test_json.c.o \
	$(O_HOST)/obj/tests-sanitised/tools/host/manifest/json.c.o $(RELIEFOS_HOST_SANITISED_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOSTCC) $(RELIEFOS_SANITISE) -g -O1 $^ -o $@

RELIEFOS_SIG_host-cc-sanitised := argv=$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) $(RELIEFOS_SANITISE) -g -O1|path=$(reliefos_host_tool_path)|identity=$(reliefos_host_tool_identity)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,host-cc-sanitised)))

-include $(shell find $(O_HOST)/obj/tests $(O_HOST)/obj/tests-sanitised -name '*.o.d' 2>/dev/null)

test-tools: $(RELIEFOS_HOST_TEST_BINS) $(RELIEFOS_HOST_TEST_SANITISED)
	@set -eu; for test_binary in $(RELIEFOS_HOST_TEST_BINS); do \
	    $(call RELIEFOS_LOG_SHELL,RUN,$$test_binary); \
	    $$test_binary; \
	done
	@set -eu; for test_binary in $(RELIEFOS_HOST_TEST_SANITISED); do \
	    $(call RELIEFOS_LOG_SHELL,RUN,$$test_binary); \
	    ASAN_OPTIONS=detect_leaks=1 \
	    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $$test_binary; \
	done

# UAPI wire-layout freeze against tools/tests/abi_layout_golden.json.
test-abi:
	$(call RELIEFOS_LOG,RUN,tools/test_abi_layout.py)
	$(Q)$(RELIEFOS_TEST_PYTHON) $(RELIEFOS_SRC)/tools/test_abi_layout.py

# headers_install boundary: exact whitelist, self-contained C/C++, no residue.
test-header-export:
	$(call RELIEFOS_LOG,RUN,tools/test_header_export.py)
	$(Q)$(RELIEFOS_TEST_PYTHON) $(RELIEFOS_SRC)/tools/test_header_export.py

# New/old UAPI dual-path compatibility (plan task 2): both include spellings
# must coexist in one translation unit and agree on layouts and constants.
test-uapi-compat: $(O_HOST)/tests/uapi_compat
	@$(call RELIEFOS_LOG_SHELL,RUN,$<)
	$(Q)$<

$(O_HOST)/tests/uapi_compat: $(RELIEFOS_SRC)/tests/uapi/uapi_compat.c $(O_META)/host-cc.sig
	$(Q)mkdir -p $(dir $@)
	$(call RELIEFOS_LOG,HOSTCC,$<)
	$(Q)$(HOSTCC) $(RELIEFOS_STRICT_WARNINGS) $(KERNEL_INCLUDES) $(HOST_CFLAGS) $< -o $@

test: test-tools test-abi test-header-export test-uapi-compat

# --- cleaning ---------------------------------------------------------------
# scripts/clean.sh enforces the ownership-marker safety rules: it never removes
# a tree it did not create, and the shared download cache survives every mode.
clean:
	@O='$(O)' SRC='$(RELIEFOS_SRC)' KEEP_CONFIG=1 sh $(RELIEFOS_SRC)/scripts/clean.sh

distclean:
	@O='$(O)' SRC='$(RELIEFOS_SRC)' KEEP_CONFIG=0 sh $(RELIEFOS_SRC)/scripts/clean.sh

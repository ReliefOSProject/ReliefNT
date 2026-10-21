# ReliefNT's standalone entry point. All C and assembly compilation, hostprogs,
# archives, command tracking and dependencies are handled by vendored Linux
# 6.12 Kbuild (scripts/Makefile.build and scripts/Makefile.host).

reliefnt_make_version := $(shell printf '4.3\n$(MAKE_VERSION)\n' | LC_ALL=C sort -V | head -n1)
ifeq ($(reliefnt_make_version),4.3)
else
$(error GNU Make >= 4.3 is required, this is $(MAKE_VERSION))
endif

RELIEFNT_SRC := $(patsubst %/,%,$(dir $(realpath $(firstword $(MAKEFILE_LIST)))))

ifeq ($(origin ARCH),undefined)
ARCH := x86_64
endif
ifeq ($(origin PROFILE),undefined)
PROFILE := release
endif
ifeq ($(origin O),undefined)
O := $(RELIEFNT_SRC)/out/$(ARCH)/$(PROFILE)
endif

ifeq ($(filter $(ARCH),x86_64),)
$(error unsupported ARCH '$(ARCH)'; this build supports ARCH=x86_64)
endif
ifeq ($(filter $(PROFILE),debug release),)
$(error unsupported PROFILE '$(PROFILE)'; use PROFILE=debug or PROFILE=release)
endif

override O := $(shell realpath -m -- '$(O)' 2>/dev/null)
ifeq ($(strip $(O)),)
$(error O= must name an output directory)
endif
ifeq ($(O),/)
$(error refusing / as the output directory)
endif
ifeq ($(O),$(RELIEFNT_SRC))
$(error refusing the source root as the output directory)
endif
ifneq ($(words $(O)),1)
$(error O='$(O)' is unsupported: spaces and newlines are not accepted in an output path)
endif

V ?= 0
ifeq ($(V),1)
Q :=
else
Q := @
endif

EXTRAVERSION =
LOCALVERSION ?=
SOURCE_DATE_EPOCH ?= $(shell git -C $(RELIEFNT_SRC) show -s --format=%ct HEAD 2>/dev/null || echo 0)
ifeq ($(origin TOOLCHAIN),command line)
# Honor an explicit caller override, but do not inherit the parent product's
# environment-selected toolchain when this child is built through integration.
else
TOOLCHAIN := $(RELIEFNT_SRC)/configs/toolchains/llvm-x86_64.mk
endif
ifeq ($(wildcard $(TOOLCHAIN)),)
$(error toolchain description '$(TOOLCHAIN)' not found)
endif
include $(TOOLCHAIN)

# GNU Make's built-in CC is not a target toolchain choice. As before, only
# explicit command-line CC/CXX/LD/AR/OBJCOPY/STRIP/NM values override the file.
reliefnt_override = $(if $(filter command line,$(origin $(2))),$($(2)),$($(1)))
TARGET_CC := $(call reliefnt_override,TOOLCHAIN_CC,CC)
TARGET_CXX := $(call reliefnt_override,TOOLCHAIN_CXX,CXX)
TARGET_AR := $(call reliefnt_override,TOOLCHAIN_AR,AR)
TARGET_RANLIB := $(call reliefnt_override,TOOLCHAIN_RANLIB,RANLIB)
TARGET_LD := $(call reliefnt_override,TOOLCHAIN_LD,LD)
TARGET_OBJCOPY := $(call reliefnt_override,TOOLCHAIN_OBJCOPY,OBJCOPY)
TARGET_STRIP := $(call reliefnt_override,TOOLCHAIN_STRIP,STRIP)
ifeq ($(origin NM),command line)
TARGET_NM := $(NM)
else
TARGET_NM := llvm-nm
endif

KERNEL_CFLAGS ?=
KERNEL_AFLAGS ?=
KERNEL_LDFLAGS ?=
HOSTCC ?= cc
HOST_CFLAGS ?= -O2 -g
HOST_LDFLAGS ?=
HEADER_EXPORT_LIST ?= $(RELIEFNT_SRC)/configs/header-export.list
RELIEFNT_TEST_PYTHON ?= python3
DESTDIR ?= $(O)/kernel-install

include $(RELIEFNT_SRC)/scripts/Makefile.reliefnt

# The output marker and persistent lock protect independent builds and make
# clean/distclean refuse directories that this checkout does not own.
RELIEFNT_O_MARKER := $(O)/.reliefos-out
RELIEFNT_O_LOCK := $(O)/.build-lock
RELIEFNT_O_ABSOLUTE := $(O)
RELIEFNT_READ_ONLY_GOALS := help
RELIEFNT_LOCK_SKIP := \
	$(if $(findstring n,$(firstword -$(MAKEFLAGS))),1) \
	$(if $(findstring q,$(firstword -$(MAKEFLAGS))),1)
ifeq ($(MAKECMDGOALS),)
RELIEFNT_LOCK_SKIP += 1
else ifeq ($(words $(filter-out $(RELIEFNT_READ_ONLY_GOALS),$(MAKECMDGOALS))),0)
RELIEFNT_LOCK_SKIP += 1
endif
ifeq ($(strip $(RELIEFNT_LOCK_SKIP)),)
RELIEFNT_BUILD_OWNER := $(shell sh $(RELIEFNT_SRC)/scripts/build-lock.sh acquire \
	'$(RELIEFNT_O_LOCK)' '$(RELIEFNT_O_ABSOLUTE)')
ifeq ($(strip $(RELIEFNT_BUILD_OWNER)),)
$(error refusing to build: '$(O)' is already being built by another make)
endif
export RELIEFOS_BUILD_OWNER := $(RELIEFNT_BUILD_OWNER)
endif

.DEFAULT_GOAL := help
.SUFFIXES:
.DELETE_ON_ERROR:

.PHONY: help all kernel kerneldebug boot vdso tools fetch fetch-check \
	defconfig olddefconfig menuconfig config-sync build-info headers_install \
	install test test-tools test-abi test-header-export test-vdso \
	kbuild-fixdep kconfig-tools clean distclean FORCE

help:
	@printf '%s\n' \
	  'ReliefNT standalone kernel build (Linux 6.12 Kbuild)' \
	  '' \
	  '  all               kernel.sys, kernel.debug, kerneldebug.sys' \
	  '  kernel            kernel.sys and kernel.debug' \
	  '  kerneldebug       kerneldebug.sys' \
	  '  vdso              build vDSO ELF and embedded object' \
	  '  test-vdso         inspect and exercise the generated vDSO' \
	  '  headers_install   export the UAPI whitelist to $(O)/kernel-export' \
	  '  install           install products and manifest.txt to DESTDIR' \
	  '  defconfig / olddefconfig / menuconfig / config-sync' \
	  '  tools / fetch / fetch-check' \
	  '  test              host tests, ABI, header export and vDSO' \
	  '  clean / distclean (download cache is preserved)' \
	  '' \
	  '  variables: O= ARCH=x86_64 PROFILE=release|debug TOOLCHAIN= V=1' \
	  '             CC= CXX= LD= AR= OBJCOPY= STRIP= NM= HOSTCC=' \
	  '             EXTRAVERSION= LOCALVERSION= SOURCE_DATE_EPOCH='

$(RELIEFNT_O_MARKER): | $(O)
	$(Q)printf 'reliefos-build-out version=1 root=%s\n' '$(RELIEFNT_SRC)' > $@.tmp
	$(Q)mv -f $@.tmp $@

$(O):
	$(Q)mkdir -p $@

FORCE:

define RELIEFNT_WRITE_TOOL_ID
set -eu; \
tool_name=$$(printf '%s\n' '$(1)' | awk '{ print $$1 }'); \
tool_path=$$(command -v "$$tool_name"); \
tool_version=$$($(1) --version 2>&1 | head -n1); \
tool_hash=$$(sha256sum "$$tool_path" | cut -d' ' -f1); \
identity=$$(printf '%s\n%s\n%s\n' '$(1)' "$$tool_version" "$$tool_hash" | sha256sum | cut -d' ' -f1); \
printf '%s\n' "$$identity" > '$@.tmp'; \
if cmp -s '$@.tmp' '$@' 2>/dev/null; then rm -f '$@.tmp'; \
else mv -f '$@.tmp' '$@'; fi
endef

$(RELIEFNT_HOST_CC_ID): FORCE $(RELIEFNT_O_MARKER)
	$(Q)set -eu; \
	mkdir -p '$(dir $(RELIEFNT_HOST_CC_ID))'; \
	tool_name=$$(printf '%s\n' '$(HOSTCC)' | awk '{ print $$1 }'); \
	tool_path=$$(command -v "$$tool_name"); \
	tool_version=$$($(HOSTCC) --version 2>&1 | head -n1); \
	tool_hash=$$(sha256sum "$$tool_path" | cut -d' ' -f1); \
	identity=$$(printf '%s\n%s\n%s\n' '$(HOSTCC)' "$$tool_version" "$$tool_hash" | sha256sum | cut -d' ' -f1); \
	printf '/* host compiler identity */\n#define RELIEFNT_HOST_COMPILER_ID "%s"\n' "$$identity" > $@.tmp; \
	if cmp -s $@.tmp $@ 2>/dev/null; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

$(RELIEFNT_TARGET_CC_ID): FORCE $(RELIEFNT_O_MARKER)
	$(Q)$(call RELIEFNT_WRITE_TOOL_ID,$(TARGET_CC))

$(RELIEFNT_TARGET_AR_ID): FORCE $(RELIEFNT_O_MARKER)
	$(Q)$(call RELIEFNT_WRITE_TOOL_ID,$(TARGET_AR))

$(RELIEFNT_TARGET_LD_ID): FORCE $(RELIEFNT_O_MARKER)
	$(Q)$(call RELIEFNT_WRITE_TOOL_ID,$(TARGET_LD))

$(RELIEFNT_TARGET_OBJCOPY_ID): FORCE $(RELIEFNT_O_MARKER)
	$(Q)$(call RELIEFNT_WRITE_TOOL_ID,$(TARGET_OBJCOPY))

kbuild-fixdep: $(RELIEFNT_O_MARKER) $(RELIEFNT_HOST_CC_ID)
	+$(Q)$(call RELIEFNT_KBUILD,obj=scripts/basic scripts/basic/fixdep)

kconfig-tools: kbuild-fixdep $(RELIEFNT_HOST_CC_ID)
	+$(Q)$(call RELIEFNT_KCONFIG,build_config build_menuconfig)

defconfig: kconfig-tools $(RELIEFNT_O_MARKER)
	+$(Q)$(call RELIEFNT_KCONFIG,defconfig)
	+$(Q)$(call RELIEFNT_KCONFIG,syncconfig)

olddefconfig: kconfig-tools $(RELIEFNT_O_MARKER)
	+$(Q)if [ ! -f '$(O)/.config' ]; then $(call RELIEFNT_KCONFIG,defconfig); fi
	+$(Q)$(call RELIEFNT_KCONFIG,olddefconfig)
	+$(Q)$(call RELIEFNT_KCONFIG,syncconfig)

menuconfig: kconfig-tools $(RELIEFNT_O_MARKER)
	+$(Q)if [ ! -f '$(O)/.config' ]; then $(call RELIEFNT_KCONFIG,defconfig); fi
	+$(Q)$(call RELIEFNT_KCONFIG,menuconfig)
	+$(Q)$(call RELIEFNT_KCONFIG,syncconfig)

config-sync: kconfig-tools $(RELIEFNT_O_MARKER)
	+$(Q)if [ ! -f '$(O)/.config' ]; then $(call RELIEFNT_KCONFIG,defconfig); fi
	+$(Q)$(call RELIEFNT_KCONFIG,syncconfig)

kernel: config-sync vdso kbuild-fixdep \
	$(RELIEFNT_TARGET_CC_ID) $(RELIEFNT_TARGET_AR_ID) $(RELIEFNT_TARGET_LD_ID) $(RELIEFNT_TARGET_OBJCOPY_ID)
	+$(Q)$(call RELIEFNT_KBUILD,obj=.)

all: kernel

kerneldebug: config-sync kbuild-fixdep \
	$(RELIEFNT_TARGET_CC_ID) $(RELIEFNT_TARGET_OBJCOPY_ID)
	+$(Q)$(call RELIEFNT_KBUILD,obj=. ./generated/system/kerneldebug.sys)

boot: kernel

vdso: config-sync kbuild-fixdep $(RELIEFNT_TARGET_CC_ID) $(RELIEFNT_TARGET_AR_ID) $(RELIEFNT_TARGET_LD_ID)
	+$(Q)$(call RELIEFNT_KBUILD,obj=vdso KBUILD_CFLAGS='$(RELIEFNT_VDSO_CFLAGS)' KBUILD_AFLAGS=)
	+$(Q)$(call RELIEFNT_KBUILD,obj=. ./vdso/vdso_blob.o)

tools: kbuild-fixdep $(RELIEFNT_HOST_CC_ID) $(RELIEFNT_O_MARKER)
	+$(Q)$(call RELIEFNT_KBUILD,obj=. $(RELIEFNT_HOST_PROGRAMS))

fetch: tools
	$(Q)$(RELIEFNT_SRC)/tools/build/fetch.sh \
		--deps $(O)/tools/host/manifest/reliefos-deps \
		--lock $(RELIEFNT_SRC)/configs/dependencies.lock.json \
		--cache $(RELIEFNT_CACHE) --only unifont

fetch-check: tools
	$(Q)$(RELIEFNT_SRC)/tools/build/fetch-check.sh \
		--deps $(O)/tools/host/manifest/reliefos-deps \
		--lock $(RELIEFNT_SRC)/configs/dependencies.lock.json \
		--only unifont

build-info: kbuild-fixdep $(RELIEFNT_HOST_CC_ID) $(RELIEFNT_O_MARKER)
	+$(Q)$(call RELIEFNT_KBUILD,obj=. ./include/generated/build_info.h)

headers_install: kbuild-fixdep $(RELIEFNT_O_MARKER)
	+$(Q)$(call RELIEFNT_KBUILD,obj=. ./kernel-export/manifest.txt)
	$(Q)set -eu; export_root='$(O)/kernel-export'; \
	while IFS= read -r relative; do \
	  case "$$relative" in \
	    \#*) continue ;; \
	  esac; \
	  source="$$export_root/.kbuild/include/$$relative"; \
	  output="$$export_root/include/$$relative"; \
	  mkdir -p "$$(dirname "$$output")"; \
	  if ! cmp -s "$$source" "$$output"; then \
	    cp "$$source" "$$output.tmp"; mv -f "$$output.tmp" "$$output"; \
	  fi; \
	done < "$$export_root/manifest.txt"

test-vdso: vdso vdso-runtime-test
	$(Q)$(RELIEFNT_TEST_PYTHON) $(RELIEFNT_SRC)/tools/test_vdso.py --output $(O) --no-build

# Tracked host suites use the same native hostprogs engine as build tools.
# Separate output contexts prevent sanitiser flags from sharing any objects.
RELIEFNT_TEST_HOST_BINS := $(O)/host/tests/test_common $(O)/host/tests/test_json
RELIEFNT_TEST_HOST_SANITISED := $(O)/host/tests-sanitised/test_common $(O)/host/tests-sanitised/test_json
RELIEFNT_TEST_SANITISE := -fsanitize=address,undefined -fno-omit-frame-pointer

define RELIEFNT_NATIVE_HOST_TESTS
.PHONY: native-host-tests-$(1)
native-host-tests-$(1): kbuild-fixdep $(RELIEFNT_HOST_CC_ID)
	$$(Q)mkdir -p $(2)
	+$$(Q)$$(MAKE) $$(RELIEFNT_KBUILD_BASE) obj=scripts/basic -C '$(2)' objtree='$(2)'
	+$$(Q)$$(MAKE) $$(RELIEFNT_KBUILD_BASE) obj=. -C '$(2)' objtree='$(2)' kbuild-file='$$(RELIEFNT_SRC)/tests/host/Kbuild' KBUILD_HOSTCFLAGS='$$(RELIEFNT_HOST_WARNINGS) $$(RELIEFNT_HOST_CPPFLAGS) -I$$(RELIEFNT_SRC)/tests/host $(3)' KBUILD_HOSTLDFLAGS='$$(HOST_LDFLAGS) $(4)'
$(addprefix $(2)/,test_common test_json): native-host-tests-$(1) ;
endef
$(eval $(call RELIEFNT_NATIVE_HOST_TESTS,plain,$(O)/host/tests,$(HOST_CFLAGS),))
$(eval $(call RELIEFNT_NATIVE_HOST_TESTS,sanitised,$(O)/host/tests-sanitised,$(RELIEFNT_TEST_SANITISE) -g -O1,$(RELIEFNT_TEST_SANITISE) -g -O1))

test-tools: $(RELIEFNT_TEST_HOST_BINS) $(RELIEFNT_TEST_HOST_SANITISED)
	$(Q)set -eu; for test_binary in $(RELIEFNT_TEST_HOST_BINS); do "$$test_binary"; done
	$(Q)set -eu; for test_binary in $(RELIEFNT_TEST_HOST_SANITISED); do \
		ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		"$$test_binary"; done

.PHONY: vdso-runtime-test
vdso-runtime-test: kbuild-fixdep $(RELIEFNT_HOST_CC_ID)
	+$(Q)$(call RELIEFNT_KBUILD,obj=. host/tests/vdso_runtime_test)

test-abi:
	$(Q)$(RELIEFNT_TEST_PYTHON) $(RELIEFNT_SRC)/tools/test_abi_layout.py

test-header-export: headers_install
	$(Q)$(RELIEFNT_TEST_PYTHON) $(RELIEFNT_SRC)/tools/test_header_export.py --out $(O)

test: test-tools test-abi test-header-export test-vdso

install: all headers_install
	$(Q)set -eu; \
	dest='$(DESTDIR)'; \
	mkdir -p "$$dest"; \
	cp $(O)/generated/system/kernel.sys \
	   $(O)/generated/system/kernel.debug \
	   $(O)/generated/system/kerneldebug.sys "$$dest"/; \
	{ \
	  printf 'format_version: 1\n'; \
	  printf 'arch: %s\n' '$(ARCH)'; \
	  printf 'profile: %s\n' '$(PROFILE)'; \
	  if git -C '$(RELIEFNT_SRC)' rev-parse --short HEAD >/dev/null 2>&1; then \
	    printf 'kernel_git: %s\n' "$$(git -C '$(RELIEFNT_SRC)' rev-parse --short HEAD)"; \
	    if [ -n "$$(git -C '$(RELIEFNT_SRC)' status --porcelain 2>/dev/null)" ]; then \
	      printf 'kernel_git_dirty: yes\n'; \
	    else printf 'kernel_git_dirty: no\n'; fi; \
	  else printf 'kernel_git: no-git\nkernel_git_dirty: unknown\n'; fi; \
	  printf 'toolchain: %s\n' "$$($(TARGET_CC) --version 2>/dev/null | head -n1)"; \
	  printf 'config_sha256: %s\n' "$$(sha256sum '$(O)/.config' | cut -d' ' -f1)"; \
	  printf 'uapi_sha256: %s\n' "$$(sha256sum '$(O)/kernel-export/manifest.txt' | cut -d' ' -f1)"; \
	  printf 'artifacts:\n'; \
	  (cd "$$dest" && sha256sum kernel.sys kernel.debug kerneldebug.sys | sed 's/^/  /'); \
	} > "$$dest/manifest.txt.tmp"; \
	mv -f "$$dest/manifest.txt.tmp" "$$dest/manifest.txt"

clean:
	$(Q)if [ -f '$(RELIEFNT_O_MARKER)' ]; then \
		O='$(O)' SRC='$(RELIEFNT_SRC)' KEEP_CONFIG=1 \
		sh $(RELIEFNT_SRC)/scripts/clean.sh; \
	else printf 'clean: no owned output at %s\n' '$(O)'; fi

distclean:
	$(Q)if [ -f '$(RELIEFNT_O_MARKER)' ]; then \
		O='$(O)' SRC='$(RELIEFNT_SRC)' KEEP_CONFIG=0 \
		sh $(RELIEFNT_SRC)/scripts/clean.sh; \
	else printf 'distclean: no owned output at %s\n' '$(O)'; fi

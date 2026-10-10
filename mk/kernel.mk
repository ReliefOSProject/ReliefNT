# Kernel, boot-loader-side freestanding objects and the linker stage.
#
# Object paths keep the component and the source-relative path so two util.c
# files cannot collide (plan section 6.1).

KERNEL_LD_SCRIPT := $(RELIEFOS_SRC)/arch/x86_64/linker.ld
# All device drivers are linked into kernel.sys itself; there are no separate
# loadable .drv modules anymore.
KERNEL_SOURCE_DIRS := kernel/reliefnt drivers/console drivers/bootstrap \
	drivers/mouse drivers/serial drivers/e1000 drivers/ac97 drivers/es1371 \
	drivers/hda arch mm fs net kernel/exec
# drivers/bootstrap/storage/*.c are textually included by the storage.c facade
# and must not also become independent objects.
KERNEL_SOURCE_EXCLUDE := drivers/bootstrap/storage

KERNEL_SOURCES_LIST := $(O_OBJ)/kernel/sources.list
KERNEL_SOURCES_MK := $(O_OBJ)/kernel/sources.mk
KERNEL_UNSTRIPPED := $(O_GENERATED)/system/kernel.unstripped
RELIEFOS_KERNEL_SYS := $(O_GENERATED)/system/kernel.sys
RELIEFOS_KERNEL_DEBUG := $(O_GENERATED)/system/kernel.debug
BUILD_INFO_HEADER := $(O_INCLUDE)/generated/build_info.h
RELIEFOS_SOURCE_ID := $(shell git -C $(RELIEFOS_SRC) rev-parse --short HEAD 2>/dev/null || echo unknown)
RELIEFOS_SIG_version := source=$(RELIEFOS_SOURCE_ID)|epoch=$(SOURCE_DATE_EPOCH)|extra=$(EXTRAVERSION)|local=$(LOCALVERSION)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,version)))

# --- flags ------------------------------------------------------------------
# Generated headers come from O/include first. The version header is generated
# only there; source-tree configuration snapshots must not shadow build outputs.
KERNEL_INCLUDES := -I$(O_INCLUDE) -I$(RELIEFOS_SRC)/kernel/reliefnt/include \
	-I$(RELIEFOS_SRC)/include/uapi -I$(RELIEFOS_SRC)/include

KERNEL_CC_BASE := $(TARGET_CC) -target $(TRIPLE_KERNEL) \
	$(RELIEFOS_OPTIMIZATION_FLAGS) -std=c11 -ffreestanding -fno-stack-protector \
	-fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=kernel \
	-Wall -Wextra -MMD -MP $(KERNEL_INCLUDES) -include $(AUTOCONF_H)

KERNEL_AS_BASE := $(TARGET_CC) -target $(TRIPLE_KERNEL) \
	$(RELIEFOS_OPTIMIZATION_FLAGS) -ffreestanding -mno-red-zone \
	-mgeneral-regs-only -MMD -MP $(KERNEL_INCLUDES)

KERNEL_LD_BASE := $(TARGET_LD) -nostdlib -z max-page-size=0x1000 \
	-T $(KERNEL_LD_SCRIPT)

# The signature records the resolved tool paths as well as the argv, so moving
# to a different clang on PATH rebuilds instead of mixing objects. Dependency
# flags are included so old output trees regenerate their depfiles with -MP.
reliefos_absolute = $(shell command -v $(1) 2>/dev/null || printf '%s' '$(1)')

RELIEFOS_SIG_kernel-cc := argv=$(KERNEL_CC_BASE) $(KERNEL_CFLAGS)|path=$(call reliefos_absolute,$(TARGET_CC))|identity=$(shell $(TARGET_CC) --version 2>&1 | head -n1)
RELIEFOS_SIG_kernel-as := argv=$(KERNEL_AS_BASE) $(KERNEL_AFLAGS)|path=$(call reliefos_absolute,$(TARGET_CC))|identity=$(shell $(TARGET_CC) --version 2>&1 | head -n1)
RELIEFOS_SIG_kernel-link := argv=$(KERNEL_LD_BASE) $(KERNEL_LDFLAGS)|path=$(call reliefos_absolute,$(TARGET_LD))|identity=$(shell $(TARGET_LD) --version 2>&1 | head -n1)

$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,kernel-cc)))
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,kernel-as)))
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,kernel-link)))

# --- source manifest --------------------------------------------------------
# The directory is the dependency: creating or removing a file changes its
# mtime, which regenerates the list, which is a prerequisite of the link. A
# deleted source therefore relinks even though every surviving object is current
# (plan section 6.1).
# reliefos-emit is what keeps the manifest mtime stable, so the rule must wait for
# it: a fresh output directory builds host tools on demand.
$(KERNEL_SOURCES_LIST): FORCE $(RELIEFOS_EMIT)
	$(Q)mkdir -p $(dir $@)
	$(Q)find $(KERNEL_SOURCE_DIRS) -path '$(KERNEL_SOURCE_EXCLUDE)/*' -prune -o \
	    -type f \( -name '*.c' -o -name '*.S' \) -print \
	  | LC_ALL=C sort > $@.tmp
	$(Q)$(RELIEFOS_EMIT) --input $@.tmp --output $@
	$(Q)rm -f $@.tmp

# The generated fragment splits the manifest by action so the two signature
# classes stay independent.
$(KERNEL_SOURCES_MK): $(KERNEL_SOURCES_LIST)
	$(Q)cc_list=''; as_list=''; \
	while read -r source; do \
	    case "$$source" in \
	        *.c) cc_list="$$cc_list $(O_OBJ)/kernel/$$source.o" ;; \
	        *)   as_list="$$as_list $(O_OBJ)/kernel/$$source.o" ;; \
	    esac; \
	done < $<; \
	printf 'RELIEFOS_KERNEL_CC_OBJS :=%s\nRELIEFOS_KERNEL_AS_OBJS :=%s\n' \
	    "$$cc_list" "$$as_list" > $@.tmp && mv $@.tmp $@

$(KERNEL_SOURCES_MK): | $(O_OBJ)

ifeq ($(RELIEFOS_PASSIVE),1)
# help, clean and distclean must not materialise or regenerate a make fragment
# they do not need (plan section 4).
else
ifneq ($(RELIEFOS_INSPECT),)
$(eval $(file <$(KERNEL_SOURCES_MK)))
else
-include $(KERNEL_SOURCES_MK)
endif
endif
# Output trees predating -MP can still reference the removed splash header.
# Allow that dependency to be read once so the compiler can replace the stale
# depfile. New depfiles emit empty header targets themselves via -MP.
$(RELIEFOS_SRC)/kernel/reliefnt/include/reliefnt/boot_splash.h \
$(RELIEFOS_SRC)/kernel/reliefnt/include/ntclks/boot_splash.h \
$(RELIEFOS_SRC)/kernel/ntclks/include/ntclks/boot_splash.h:

-include $(shell find $(O_OBJ)/kernel -name '*.o.d' 2>/dev/null)

RELIEFOS_KERNEL_OBJS := $(RELIEFOS_KERNEL_CC_OBJS) $(RELIEFOS_KERNEL_AS_OBJS)

# --- compilation ------------------------------------------------------------
# One generated header per object, not one per build: the storage facade and the
# two special dependencies below keep the graph honest without making every
# object wait for every asset.
$(O_OBJ)/kernel/%.c.o: $(RELIEFOS_SRC)/%.c $(AUTOCONF_H) $(O_META)/kernel-cc.sig
	$(Q)mkdir -p $(dir $@)
	$(call RELIEFOS_LOG,CC,$<)
	$(Q)$(KERNEL_CC_BASE) $(KERNEL_CFLAGS) -MF $@.d -c $< -o $@

$(O_OBJ)/kernel/%.S.o: $(RELIEFOS_SRC)/%.S $(O_META)/kernel-as.sig
	$(Q)mkdir -p $(dir $@)
	$(call RELIEFOS_LOG,AS,$<)
	$(Q)$(KERNEL_AS_BASE) $(KERNEL_AFLAGS) -MF $@.d -c $< -o $@

# Extra prerequisites for the objects that consume generated headers. Depfiles
# take over from the second build onward; these make the first build correct.
$(O_OBJ)/kernel/kernel/reliefnt/version.c.o: $(BUILD_INFO_HEADER)

# The storage facade textually includes its private modules.
$(O_OBJ)/kernel/drivers/bootstrap/storage.c.o: \
	$(wildcard $(RELIEFOS_SRC)/drivers/bootstrap/storage/*.c) \
	$(RELIEFOS_SRC)/drivers/bootstrap/storage/storage_internal.h

# --- generated inputs -------------------------------------------------------
$(BUILD_INFO_HEADER): $(RELIEFOS_SRC)/configs/build-version $(RELIEFOS_VERSION_TOOL) $(O_META)/version.sig \
	| $(O_INCLUDE)/generated
	$(call RELIEFOS_LOG,GEN,$@)
	$(Q)$(RELIEFOS_VERSION_TOOL) --version-file $< \
        --extra-version '$(EXTRAVERSION)' --local-version '$(LOCALVERSION)' \
        --source-id '$(RELIEFOS_SOURCE_ID)' \
	    --epoch '$(or $(SOURCE_DATE_EPOCH),$(shell git -C $(RELIEFOS_SRC) show -s --format=%ct HEAD 2>/dev/null || echo 0))' \
	    --output $@

# --- link and images --------------------------------------------------------
# Grouped target: kernel.unstripped is the only output of the link, but the two
# objcopy products are derived from it independently, so each gets its own rule
# and neither can be mistaken for a complete-image stamp.
$(KERNEL_UNSTRIPPED): $(RELIEFOS_KERNEL_OBJS) $(KERNEL_LD_SCRIPT) \
	$(KERNEL_SOURCES_LIST) $(O_META)/kernel-link.sig | $(O_GENERATED)/system
	$(Q)mkdir -p $(dir $@)
	$(call RELIEFOS_LOG,LD,$@)
	$(Q)$(KERNEL_LD_BASE) $(KERNEL_LDFLAGS) -o $@ \
	    $(RELIEFOS_KERNEL_CC_OBJS) $(RELIEFOS_KERNEL_AS_OBJS)

$(RELIEFOS_KERNEL_SYS): $(KERNEL_UNSTRIPPED) $(O_META)/kernel-objcopy.sig | $(O_GENERATED)/system
	$(call RELIEFOS_LOG,IMAGE,$@)
	$(Q)$(TARGET_OBJCOPY) --strip-debug $< $@.tmp
	$(Q)mv $@.tmp $@

$(RELIEFOS_KERNEL_DEBUG): $(KERNEL_UNSTRIPPED) $(O_META)/kernel-objcopy.sig | $(O_GENERATED)/system
	$(call RELIEFOS_LOG,IMAGE,$@)
	$(Q)$(TARGET_OBJCOPY) --only-keep-debug $< $@.tmp
	$(Q)mv $@.tmp $@

RELIEFOS_SIG_kernel-objcopy := argv=$(TARGET_OBJCOPY)|path=$(call reliefos_absolute,$(TARGET_OBJCOPY))|identity=$(shell $(TARGET_OBJCOPY) --version 2>&1 | head -n1)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,kernel-objcopy)))

# Whole-object-tree invalidation guard: `.DELETE_ON_ERROR` covers interrupted
# recipes, and the object files are individually tracked by the depfiles above.
.DELETE_ON_ERROR:

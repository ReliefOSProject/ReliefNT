# Generated resource headers consumed by the kernel build.
#
# Only the CJK font pipeline migrates with the kernel: cjk_font.h feeds
# drivers/video/framebuffer/framebuffer.c and is the one generated asset in the kernel
# link (migration manifest section 3, hazard 5). The unifont dependency
# closure travels with it (configs/dependencies.lock.json, tools/build/fetch.sh,
# tools/host/manifest/reliefos-deps, resources/licenses/unifont-LICENSE). The UI
# and GRUB font rules stay with the parent repository.

RELIEFOS_CJK_FONT_TOOL := $(RELIEFOS_HOST_BIN)/reliefos-cjk-font
$(RELIEFOS_CJK_FONT_TOOL): $(O_HOST)/obj/tools/host/assets/reliefos-cjk-font.c.o | $(RELIEFOS_HOST_BIN)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@

$(O_INCLUDE)/generated/cjk_font.h: $(RELIEFOS_CJK_FONT_TOOL) $(RELIEFOS_DEPS_TOOL) $(RELIEFOS_SRC)/configs/dependencies.lock.json $(RELIEFOS_SRC)/mk/resources.mk
	$(Q)sh $(RELIEFOS_SRC)/tools/build/fetch.sh --deps $(RELIEFOS_DEPS_TOOL) --lock $(RELIEFOS_SRC)/configs/dependencies.lock.json --cache $(RELIEFOS_CACHE) --only unifont --verify-only
	$(Q)mkdir -p $(@D)
	$(Q)set -eu; font=$$($(RELIEFOS_DEPS_TOOL) --lock $(RELIEFOS_SRC)/configs/dependencies.lock.json --id unifont --print directory); gzip -dc $(RELIEFOS_CACHE)/$$font > $@.hex; $(RELIEFOS_CJK_FONT_TOOL) < $@.hex > $@.tmp; mv $@.tmp $@; rm -f $@.hex

$(O_OBJ)/kernel/drivers/video/framebuffer/framebuffer.c.o: $(O_INCLUDE)/generated/cjk_font.h
tools: $(RELIEFOS_CJK_FONT_TOOL)

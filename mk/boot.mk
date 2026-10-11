# Complete freestanding boot chain. No pre-existing build/ artifacts are inputs.
# GRUB loads kernel.sys directly through its Multiboot2 header; the only boot
# product built here is the standalone kernel debug module.
KERNELDEBUG_SYS := $(O_GENERATED)/system/kerneldebug.sys
$(KERNELDEBUG_SYS): $(RELIEFOS_SRC)/debug/kerneldebug.c $(AUTOCONF_H) $(O_META)/kernel-cc.sig
	$(Q)mkdir -p $(@D)
	$(Q)$(KERNEL_CC_BASE) -c $< -o $@.o.tmp
	$(Q)$(TARGET_OBJCOPY) --remove-section .llvm_addrsig --remove-section .comment --remove-section .note.GNU-stack --rename-section .note.reliefos.kerneldebug=.note.reliefos.kerneldebug,alloc,load,readonly,data,contents $@.o.tmp $@.tmp
	$(Q)mv $@.tmp $@
	$(Q)rm $@.o.tmp
.PHONY: kerneldebug boot
kerneldebug: $(KERNELDEBUG_SYS)
boot: kernel kerneldebug

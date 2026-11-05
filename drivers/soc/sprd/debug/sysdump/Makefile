obj-$(CONFIG_SPRD_SYSDUMP) += sysdump.o
# With CONFIG_KEXEC=y, CRASH_CORE=y builds kernel/crash_core.c, which
# provides every symbol this vendor copy defines (it IS a copy of it), so
# building both is a duplicate-symbol link error.  CRASH_CORE only exists
# when KEXEC is enabled, which the stock config does not do.
ifeq ($(CONFIG_CRASH_CORE),y)
obj-$(CONFIG_SPRD_MINI_SYSDUMP) +=
else
obj-$(CONFIG_SPRD_MINI_SYSDUMP) += sprd_vmcoreinfo.o
endif

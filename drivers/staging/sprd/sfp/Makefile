GCOV_PROFILE 			= ${CONFIG_SPRD_SFP_GCOV}
obj-$(CONFIG_SPRD_SFP_SUPPORT)  += sfp_mgr.o
obj-$(CONFIG_SPRD_SFP_SUPPORT)  += sfp_fwd.o
obj-$(CONFIG_SPRD_SFP_SUPPORT)  += sfp_intf.o
obj-$(CONFIG_SPRD_SFP_SUPPORT)  += sfp_proc.o
obj-$(CONFIG_SPRD_SFP_SUPPORT)  += sfp_tuple.o
obj-$(CONFIG_SPRD_SFP_SUPPORT)  += sysctl_net_sfp.o
obj-$(CONFIG_SPRD_SFP_SUPPORT)  += sfp_ct.o
obj-$(CONFIG_SPRD_SFP_SUPPORT)  += sfp_hook.o
obj-$(CONFIG_SPRD_SFP_SUPPORT)  += sfp_ipa.o
obj-$(CONFIG_SPRD_IPA_V3_SUPPORT)  += sfp_netlink.o
obj-$(CONFIG_SPRD_IPA_V3_SUPPORT)  += sfp_filter.o
obj-$(CONFIG_SPRD_SFP_TEST)  += sfp_test.o

# sfp_tuple.c includes <sfp.h>, and sfp.h itself includes "sfp_hash.h";
# the shipping kernel builds this driver (102 sfp_* symbols in System.map),
# so the published Makefile is simply missing its own directory on the
# include path.
ccflags-y += -I$(src)

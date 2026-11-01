obj-$(CONFIG_DEVFREQ_SPRD_AUTO_DFS)     += sprd_dfs_auto_drv.o
obj-$(CONFIG_DEVFREQ_GOV_SPRD_SCENE)     += sprd_scene_governor.o
obj-$(CONFIG_DEVFREQ_SPRD_EXT_VOTE_UMS512)     += sprd_dfs_vote_ums512.o
obj-$(CONFIG_DEVFREQ_SPRD_EXT_VOTE_ROC1)     += sprd_dfs_vote_roc1.o

# sprd_dfs_trace.h uses TRACE_INCLUDE_PATH "."; the trace machinery
# includes "./sprd_dfs_trace.h" from include/trace/define_trace.h, so the
# source directory must be on the include path (same pattern as
# drivers/gpu/drm/sprd).
ccflags-y += -I$(src)

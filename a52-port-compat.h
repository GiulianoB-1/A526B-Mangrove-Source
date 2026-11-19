#ifndef A52_PORT_COMPAT_H
#define A52_PORT_COMPAT_H
#define CONFIG_ARCH_LITO 1
#define CONFIG_ARCH_LAGOON 1
#define CONFIG_DRM_MSM 1
#define CONFIG_DRM_MSM_SDE 1
#define CONFIG_DRM_SDE_RSC 1
#define CONFIG_DRM_MSM_DSI 1
#define CONFIG_DSI_PARSER 1
#define CONFIG_DISPLAY_SAMSUNG 1
#define CONFIG_PANEL_S6E3FC3_AMS646YD01_FHD 1
#define CONFIG_QCOM_KGSL 1
#define CONFIG_QCOM_KGSL_IOMMU 1
#define CONFIG_QCOM_MDSS_PLL 1
#define CONFIG_QSEECOM 1
#define CONFIG_QCOM_QSEECOM 1
#define CONFIG_MSM_QTEE_SHMBRIDGE 1
#define CONFIG_MSM_SMCINVOKE 1
#define CONFIG_ION 1
#define CONFIG_ION_SYSTEM_HEAP 1

/* A52_PHASE2_NATIVE_API_INCLUDES */
#include <linux/interrupt.h>
#include <linux/timekeeping.h>
#include <linux/time64.h>
#include <linux/dma-resv.h>
#include <linux/fs.h>

/* A52 legacy V4L2 video-device type name. */
#define VFL_TYPE_GRABBER VFL_TYPE_VIDEO

/* A52_PHASE7_MMIO_DRM_INCLUDES */
#include <linux/io.h>
#include <drm/drm_gem.h>
#include <drm/drm_fourcc.h>


/* A52_PHASE7_MMIO_DRM_HELPERS */
/*
 * Samsung 4.19 could bypass its MSM RTB I/O logging wrapper. Android 5.10
 * has no equivalent logging layer, so the native relaxed write is the exact
 * remaining access primitive.
 */
#ifndef writel_relaxed_no_log
#define writel_relaxed_no_log(value, address) \
	writel_relaxed((value), (address))
#endif

/* The unlocked GEM put helper was consolidated into drm_gem_object_put(). */
#ifndef drm_gem_object_put_unlocked
#define drm_gem_object_put_unlocked(object) drm_gem_object_put(object)
#endif

static inline int a52_drm_format_num_planes(u32 format)
{
	const struct drm_format_info *info = drm_format_info(format);

	return info ? info->num_planes : 1;
}

static inline int a52_drm_format_horz_chroma_subsampling(u32 format)
{
	const struct drm_format_info *info = drm_format_info(format);

	return info ? info->hsub : 1;
}

static inline int a52_drm_format_vert_chroma_subsampling(u32 format)
{
	const struct drm_format_info *info = drm_format_info(format);

	return info ? info->vsub : 1;
}

#define drm_format_num_planes(format) \
	a52_drm_format_num_planes(format)
#define drm_format_horz_chroma_subsampling(format) \
	a52_drm_format_horz_chroma_subsampling(format)
#define drm_format_vert_chroma_subsampling(format) \
	a52_drm_format_vert_chroma_subsampling(format)

/* A52_PHASE8_CACHE_INCLUDES */
#include <asm/cacheflush.h>


/* A52_PHASE8_CACHE_RANGE_HELPERS */
/*
 * Samsung 4.19 exposed dmac_*_range wrappers around the ARM64 cache
 * maintenance primitives. Android 5.10 retains the same public operations
 * under their architectural names, but removed the legacy wrappers.
 */
#define A52_CACHE_RANGE_SIZE(start, end) \
	((size_t)((char *)(end) - (char *)(start)))

#ifndef dmac_flush_range
#define dmac_flush_range(start, end) \
	__dma_flush_area((void *)(start), A52_CACHE_RANGE_SIZE(start, end))
#endif

#ifndef dmac_inv_range
#define dmac_inv_range(start, end) \
	__inval_dcache_area((void *)(start), A52_CACHE_RANGE_SIZE(start, end))
#endif

#ifndef dmac_clean_range
#define dmac_clean_range(start, end) \
	__clean_dcache_area_poc((void *)(start), A52_CACHE_RANGE_SIZE(start, end))
#endif

/* A52_PHASE9_HIGHMEM_INCLUDES */
#include <linux/highmem.h>


/* A52_PHASE9_HIGHMEM_MMIO_HELPERS */
/* Samsung's MSM RTB logging layer is absent from Android 5.10. */
#ifndef readl_relaxed_no_log
#define readl_relaxed_no_log(address) readl_relaxed((address))
#endif

/* ARM64 direct mappings made this helper a no-op in the TouchGrass source. */
#ifndef kmap_atomic_flush_unused
#define kmap_atomic_flush_unused() do { } while (0)
#endif

/* A52_PHASE10_DDR_TOPOLOGY_INCLUDES */
#include <linux/of.h>


/* A52_PHASE10_DDR_TOPOLOGY_HELPERS */
/*
 * TouchGrass read these values from the flat /memory node. All current A52
 * consumers execute after the live device tree exists, so use the equivalent
 * OF property API retained by Android 5.10.
 */
static inline int of_fdt_get_ddrtype(void)
{
	struct device_node *memory;
	u32 value;
	int ret = -ENOENT;

	memory = of_find_node_by_path("/memory");
	if (memory && !of_property_read_u32(memory, "ddr_device_type", &value))
		ret = (int)value;
	of_node_put(memory);
	return ret;
}

static inline int of_fdt_get_ddrrank(int channel)
{
	struct device_node *memory;
	char property[32];
	u32 value;
	int ret = -ENOENT;

	snprintf(property, sizeof(property), "ddr_device_rank_ch%d", channel);
	memory = of_find_node_by_path("/memory");
	if (memory && !of_property_read_u32(memory, property, &value))
		ret = (int)value;
	of_node_put(memory);
	return ret;
}

static inline int of_fdt_get_ddrhbb(int channel, int rank)
{
	struct device_node *memory;
	char property[40];
	u32 value;
	int ret = -ENOENT;

	snprintf(property, sizeof(property), "ddr_device_hbb_ch%d_rank%d",
		 channel, rank);
	memory = of_find_node_by_path("/memory");
	if (memory && !of_property_read_u32(memory, property, &value))
		ret = (int)value;
	of_node_put(memory);
	return ret;
}
/* A52_PHASE13_ALL_KNOWN_COMPAT_INCLUDES */
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/dma-buf.h>
#include <linux/dma-map-ops.h>
#include <linux/mutex.h>
#include <linux/coresight.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_prime.h>

/* A52_PHASE13_ALL_KNOWN_COMPAT_SHIMS: diagnostic, non-flashable. */
#ifndef DRIVER_PRIME
#define DRIVER_PRIME 0
#endif
#ifndef CONFIG_QCOM_ADRENO_DEFAULT_GOVERNOR
#define CONFIG_QCOM_ADRENO_DEFAULT_GOVERNOR "msm-adreno-tz"
#endif
#ifndef devm_ioremap_nocache
#define devm_ioremap_nocache(d,o,z) devm_ioremap((d),(o),(z))
#endif
#ifndef DRM_DEBUG_DRIVER_RATELIMITED
#define DRM_DEBUG_DRIVER_RATELIMITED(f,...) DRM_DEBUG_DRIVER(f,##__VA_ARGS__)
#endif
#ifndef __drm_atomic_helper_disable_plane
#define __drm_atomic_helper_disable_plane(p,c) drm_atomic_helper_disable_plane((p),(c))
#endif
#ifndef __drm_atomic_helper_set_config
#define __drm_atomic_helper_set_config(s,c) drm_atomic_helper_set_config((s),(c))
#endif
#ifndef drm_plane_helper_disable
#define drm_plane_helper_disable(p,c) drm_atomic_helper_disable_plane((p),(c))
#endif
#ifndef drm_bridge_disable
#define drm_bridge_disable(b) drm_bridge_chain_disable((b))
#define drm_bridge_post_disable(b) drm_bridge_chain_post_disable((b))
#define drm_bridge_mode_set(b,m,a) drm_bridge_chain_mode_set((b),(m),(a))
#define drm_bridge_pre_enable(b) drm_bridge_chain_pre_enable((b))
#define drm_bridge_enable(b) drm_bridge_chain_enable((b))
#endif
#ifndef drm_mode_create_colorspace_property
#define drm_mode_create_colorspace_property(c) drm_mode_create_dp_colorspace_property((c))
#endif
static inline int a52_cpp(u32 f,int p){const struct drm_format_info*i=drm_format_info(f);return(!i||p<0||p>=i->num_planes)?0:i->cpp[p];}
#ifndef drm_format_plane_cpp
#define drm_format_plane_cpp(f,p) a52_cpp((f),(p))
#endif
static inline void*a52_kmap(struct dma_buf*b,unsigned long n){void*v=dma_buf_vmap(b);return v?(char*)v+n*PAGE_SIZE:NULL;}
static inline void a52_kunmap(struct dma_buf*b,unsigned long n,void*v){if(v)dma_buf_vunmap(b,(char*)v-n*PAGE_SIZE);}
#ifndef dma_buf_kmap
#define dma_buf_kmap(b,n) a52_kmap((b),(n))
#define dma_buf_kunmap(b,n,v) a52_kunmap((b),(n),(v))
#endif
#ifndef dma_release_declared_memory
#define dma_release_declared_memory(d) dma_release_coherent_memory((d))
#endif
static inline struct task_struct*a52_owner(struct mutex*l){return(struct task_struct*)((unsigned long)atomic_long_read(&l->owner)&~0x7UL);}
#ifndef __mutex_owner
#define __mutex_owner(l) a52_owner((l))
#endif
static inline int a52_nb(struct notifier_block*n){return 0;}
static inline int a52_reclaim(struct address_space*m,void*d){return NOTIFY_DONE;}
#define show_mem_extra_notifier_register(n) a52_nb((n))
#define show_mem_extra_notifier_unregister(n) a52_nb((n))
#define proc_reclaim_notifier_register(n) a52_nb((n))
#define proc_reclaim_notifier_unregister(n) a52_nb((n))
#define reclaim_address_space(m,d) a52_reclaim((m),(d))
#define sched_set_refresh_rate(r) do{}while(0)
/* A52_PHASE423_RPMH_SOLVER_MODE_COMPAT_V1
 * Restore the legacy SDE -> RPMh software solver-state notification only.
 * Hardware solver programming remains owned by sde_rsc_hw state_update().
 */
int a52_rpmh_mode_solver_set_compat(const struct device *dev, bool enable);
#define rpmh_mode_solver_set(d,e) a52_rpmh_mode_solver_set_compat((d), (e))
/* A52_PHASE305_DISPLAY_RPMH_FLUSH_COMPAT_V1: flush-only causal repair. */
int a52_rpmh_flush_compat(const struct device *dev);
#define rpmh_flush(d) a52_rpmh_flush_compat((d))


/* A52_PHASE13_SECONDARY_COMPAT: compile-only follow-up. */
#ifndef dma_buf_set_destructor
#define dma_buf_set_destructor(buf, dtor, data) do { } while (0)
#endif

#endif

// SPDX-License-Identifier: GPL-2.0-only
/* A52_PHASE445_RECOVERY_LADDER_V1
 * Matched passive DSI/SDE state recorder for the first natural F0 5A 5A.
 * No reset, force-clock, FIFO substitution, or synthetic panel command.
 */
#include <linux/atomic.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/genhd.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/random.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>

#include "dsi/dsi_ctrl.h"
#include "dsi/dsi_display.h"
#include "dsi/dsi_panel.h"
#include "dsi/dsi_phy.h"
#if defined(CONFIG_DISPLAY_SAMSUNG)
#include "samsung/ss_dsi_panel_common.h"
#endif
#include "dsi/dsi_ctrl_hw.h"
#include "dsi/dsi_clk.h"
#include "dsi/dsi_ctrl_reg.h"
#include "dsi/dsi_hw.h"
#include <linux/kthread.h>
#include <linux/wait.h>

#define P445_KIND 1U
#define P445_IMAGE_BYTES 0x00098000U
/* A52_PHASE446U_P445_240_SECTIONS_V1 */
#define P445_HEADER_BYTES SZ_16K
#define P445_MAX_SECTIONS 240U
#define P445_MAX_HOT 27U
#define P445_RAM_PHYS 0xB1A62000ULL
#define P445_RAM_BYTES 0x00098000U
#define P445_DEBUG_OFFSET 0x00000000ULL
#define P445_DISK_ENABLED 0
#define P445_STORAGE_V2 1
#define P445_MAGIC 0x353434504d4f4346ULL /* FCOMP445 */
#define P445_VERSION 2U
#define P445_HDR_COMMIT 0x445b0071U
#define P445_SEC_COMMIT 0x445c0de5U
#define P445_STAGE_PRE 1U
#define P445_STAGE_R0_HOT 2U
#define P445_STAGE_R0_TERM 3U
#define P445_STAGE_R1_PRE 4U
#define P445_STAGE_R1_HOT 5U
#define P445_STAGE_R1_TERM 6U
#define P445_STAGE_R2_PRE 7U
#define P445_STAGE_R2_HOT 8U
#define P445_STAGE_R2_TERM 9U
#define P445_STAGE_CENSUS 10U
#define P445_STAGE_SD 11U
#define P445_GATE_OFF 0U
#define P445_GATE_BLOCK 1U
#define P445_GATE_DIAG 2U
#define P445_WIN_NONE 0U
#define P445_WIN_R0 1U
#define P445_WIN_R1 2U
#define P445_WIN_R2 3U
#define P445_R2F_HANDOFF_STUCK BIT(0)
#define P445_R2F_VCO_PREP_STUCK BIT(1)
#define P445_R2F_LINK_REF_STUCK BIT(2)
#define P445_TYPE_META 1U
#define P445_TYPE_DSI_DIRECT 2U
#define P445_TYPE_PHY 3U
#define P445_TYPE_SDE_REG 4U
#define P445_TYPE_SDE_BUS 5U
#define P445_TYPE_VBIF_BUS 6U
#define P445_TYPE_DSI_BUS 7U
#define P445_TYPE_HOT 8U
#define P445_TYPE_SD 9U
#define P445_F_RECOMPILE BIT(0)
#define P445_F_OVERFLOW BIT(1)
#define P445_F_RAM_OK BIT(2)
#define P445_F_DISK_OK BIT(3)
#define P445_F_DISK_DISABLED_OVERLAP BIT(4)

struct p445_section {
    char name[24];
    u32 stage, type, offset, length, crc32, zero_count, ffff_count;
    u32 aux0, aux1, commit;
} __packed;

struct p445_header {
    u64 magic;
    u32 version, kind, flags, state;
    u64 boot_id, init_ns, predeep_ns, pretrigger_ns, trigger_ns, terminal_ns;
    s32 terminal_ret;
    u32 terminal_irq, section_count, used_bytes;
    u32 frontier_stage, frontier_item;
    s32 disk_submit_rc, disk_flush_rc;
    u32 disk_generation, disk_written_generation, disk_retries;
    u32 hdr_crc32, commit;
    u32 gate, rung, winner, r2_valid;
    s32 r0_ret, r1_ret, r2_ret;
    u32 r0_irq, r1_irq, r2_irq;
    u32 r0_lane, r1_lane, r2_lane;
    u32 panel_initialized, panel_ulps_feature, panel_ulps_suspend, samsung_panel_state;
    u32 core_ref, link_ref, core_clk_state, link_clk_state;
    u32 pll_handoff, pll_on, pll_prepare, pll_enable, pll_lock, pll_resource_ref;
    u32 refgen_registered, refgen_enabled, refgen_use_count, refgen_pwrdwn;
    u32 r2_flags;
    u32 dropped_sections, section_capacity;
    u8 reserved[56];
    struct p445_section sec[P445_MAX_SECTIONS];
} __packed;

struct p445_pair { u32 key, value; } __packed;
struct p445_hot {
    u64 dt_ns;
    u32 point, status, clk_status, int_ctrl, lane_status, dbg171;
    u32 crc32;
} __packed;
struct p445_sdrec {
    u32 minor, partno;
    u64 sectors;
    char name[24];
} __packed;

static u8 *p445_image;
static void __iomem *p445_ram;
static void __iomem *p445_phy;
static atomic_t p445_used = ATOMIC_INIT(P445_HEADER_BYTES);
static atomic_t p445_sections = ATOMIC_INIT(0);
static atomic_t p445_state = ATOMIC_INIT(0); /* 0 idle, 1 armed, 2 final */
static atomic_t p445_gate = ATOMIC_INIT(P445_GATE_OFF);
static atomic_t p445_rung = ATOMIC_INIT(0);
static atomic_t p445_dropped = ATOMIC_INIT(0);
static atomic_t p445_disk_gen = ATOMIC_INIT(0);
static atomic_t p445_disk_written = ATOMIC_INIT(0);
static atomic_t p445_disk_retry = ATOMIC_INIT(0);
static struct dsi_ctrl *p445_ctrl;
static struct p445_hot p445_hot[P445_MAX_HOT];
static atomic_t p445_hot_count = ATOMIC_INIT(0);
static u32 p445_saved_dbg;

/* A52_PHASE446I_MATCHED_PASSIVE_F0_TWIN_V1
 * Matched GKI/TG F0 environment capture. Hot-path code is MMIO-only.
 */
static const char p446i_marker[] __used =
    "A52_PHASE446I_MATCHED_PASSIVE_F0_TWIN_V1";
#define P446I_REFGEN_PHYS 0x088e7000ULL
#define P446I_REFGEN_BYTES 0x100U
#define P446I_DISPCC_PHYS 0x0af00000ULL
#define P446I_DISPCC_BYTES 0x3000U
#define P446I_MDP_PHYS 0x0ae00000ULL
#define P446I_MDP_BYTES 0x85000U

#define P446I_STAGE_SW   0x44610001U
#define P446I_STAGE_PRE  0x44610002U
#define P446I_STAGE_POST 0x44610003U
#define P446I_TYPE_RAW 10U
#define P446I_TYPE_BUS 11U
#define P446I_TYPE_SW 12U

struct p446i_bus_pair { u32 selector, value; } __packed;
struct p446i_sw {
    u32 flags, msg_flags, panel_mode, power_state;
    u32 host_initialized, controller_state, cmd_engine_state, vid_engine_state;
    u32 byte_rate, pix_rate, byte_intf_rate, esc_rate;
} __packed;
struct p446i_raw {
    u64 ns;
    u32 dsi_status, lane_status, clk_status, int_ctrl, dbg_ctl;
    u32 refgen[64];
    u32 rcg[30];
    u32 pll0[9];
    u32 mdp[12];
    u32 compact[8];
} __packed;

static void __iomem *p446i_refgen, *p446i_dispcc, *p446i_mdp;
static atomic_t p446i_seen = ATOMIC_INIT(0);
static atomic_t p446i_target = ATOMIC_INIT(0);
static struct p446i_sw p446i_sw;
static struct p446i_raw p446i_pre, p446i_post;
static struct p446i_bus_pair p446i_bus_pre[132];
static struct p446i_bus_pair p446i_bus_post[132];
static const u32 p446i_sel[132] = { 0x0001U, 0x1001U, 0x0001U, 0x0011U, 0x1021U, 0x0021U, 0x0031U, 0x0041U, 0x0051U, 0x0061U, 0x3061U, 0x0061U, 0x2061U, 0x2061U, 0x1061U, 0x1061U, 0x1061U, 0x0071U, 0x0071U, 0x0071U, 0x0081U, 0x0081U, 0x00a1U, 0x00a1U, 0x10a1U, 0x20a1U, 0x30a1U, 0x10a1U, 0x10a1U, 0x30a1U, 0x20a1U, 0x00b1U, 0x00c1U, 0x00c1U, 0x10c1U, 0x20c1U, 0x30c1U, 0x00d1U, 0x00d1U, 0x20d1U, 0x30d1U, 0x00e1U, 0x00e1U, 0x00e1U, 0x00f1U, 0x00f1U, 0x0101U, 0x0101U, 0x1101U, 0x2101U, 0x3101U, 0x0111U, 0x0141U, 0x1141U, 0x0141U, 0x1141U, 0x1141U, 0x0151U, 0x0151U, 0x1151U, 0x2151U, 0x3151U, 0x0161U, 0x0161U, 0x1161U, 0x0171U, 0x0171U, 0x0181U, 0x0181U, 0x0191U, 0x0191U, 0x01a1U, 0x01a1U, 0x01b1U, 0x01b1U, 0x11b1U, 0x21b1U, 0x01c1U, 0x01c1U, 0x11c1U, 0x21c1U, 0x31c1U, 0x01d1U, 0x01d1U, 0x01d1U, 0x01d1U, 0x11d1U, 0x21d1U, 0x21d1U, 0x01e1U, 0x01e1U, 0x01f1U, 0x01f1U, 0x0201U, 0x0201U, 0x0211U, 0x0221U, 0x0231U, 0x0241U, 0x0251U, 0x0281U, 0x0291U, 0x0281U, 0x0291U, 0x02a1U, 0x02b1U, 0x02c1U, 0x0321U, 0x0321U, 0x1321U, 0x2321U, 0x3321U, 0x0331U, 0x0331U, 0x1331U, 0x0341U, 0x0341U, 0x1341U, 0x2341U, 0x3341U, 0x0351U, 0x0361U, 0x0361U, 0x1361U, 0x2361U, 0x0371U, 0x0381U, 0x0391U, 0x03c1U, 0x03d1U, 0x03e1U, 0x03f1U };
static const u32 p446i_compact_sel[8] = {
    0x0151U,0x0171U,0x0181U,0x0191U,0x01a1U,0x01b1U,0x01c1U,0x0211U
};
static const u32 p446i_rcg_base[6] = {
    0x107cU,0x10c4U,0x1064U,0x10e0U,0x10acU,0x115cU
};
static const u32 p446i_pll_off[9] = {
    0x00U,0x04U,0x08U,0x0cU,0x10U,0x14U,0x18U,0x1cU,0x2cU
};
static struct dsi_ctrl_cmd_dma_info p445_saved_cmd_mem;
static u32 p445_saved_hw_flags;
static bool p445_saved_cmd_valid;
static struct mipi_dsi_msg p445_saved_msg;
static u8 p445_saved_payload[3];

extern int a52_p445_regulator_state(struct regulator *reg, int *enabled,
        int *use_count, int *voltage);
extern int a52_p445_refgen_snapshot(u32 *registered, u32 *enabled,
        u32 *use_count, u32 *pwrdwn);
extern int a52_p445_clk_client_snapshot(void *handle, u32 *core_ref,
        u32 *link_ref, u32 *core_state, u32 *link_state);
extern int a52_p445_pll10_snapshot(u32 idx, u32 *handoff, u32 *pll_on,
        u32 *prepare_count, u32 *enable_count, u32 *lock_status,
        u32 *resource_ref);
extern int a52_p445_deferred_snapshot(char *buf, u32 stride, u32 max);

extern void a52_p445_sde_capture(u32 stage, struct dsi_ctrl_hw *ctrl);

static u32 p445_crc(const void *p, size_t n)
{
    return crc32_le(~0U, p, n) ^ ~0U;
}

static struct p445_header *p445_hdr(void)
{
    return (struct p445_header *)p445_image;
}

static void p445_flush_ram(void __iomem *p, size_t n)
{
#if P445_KIND == 1
    if (!p || !n) return;
    wmb();
    __flush_dcache_area((void __force *)p, n);
    dsb(sy);
#else
    (void)p; (void)n;
#endif
}

static void p445_sync_header(void)
{
    struct p445_header *h;
    if (!p445_image) return;
    h = p445_hdr();
    h->section_count = (u32)atomic_read(&p445_sections);
    h->used_bytes = (u32)atomic_read(&p445_used);
    h->dropped_sections = (u32)atomic_read(&p445_dropped);
    h->section_capacity = P445_MAX_SECTIONS;
    h->state = (u32)atomic_read(&p445_state);
    h->disk_generation = (u32)atomic_read(&p445_disk_gen);
    h->disk_written_generation = (u32)atomic_read(&p445_disk_written);
    h->disk_retries = (u32)atomic_read(&p445_disk_retry);
    h->hdr_crc32 = 0;
    h->commit = P445_HDR_COMMIT;
    h->hdr_crc32 = p445_crc(h, offsetof(struct p445_header, hdr_crc32));
#if P445_KIND == 1
    if (p445_ram) {
        memcpy_toio(p445_ram, p445_image, P445_HEADER_BYTES);
        p445_flush_ram(p445_ram, P445_HEADER_BYTES);
    }
#endif
}

void a52_p445_frontier(u32 stage, u32 item)
{
    if (!p445_image) return;
    WRITE_ONCE(p445_hdr()->frontier_stage, stage);
    WRITE_ONCE(p445_hdr()->frontier_item, item);
    p445_sync_header();
#if P445_KIND == 2
    pr_info("P445 F st=%u/%u item=%u/%u crc=%08x\n",
        stage, stage, item, item,
        p445_crc(&p445_hdr()->frontier_stage, sizeof(u32) * 2));
#endif
}
EXPORT_SYMBOL_GPL(a52_p445_frontier);

int a52_p445_store_section(u32 stage, u32 type, const char *name,
                           u32 aux0, u32 aux1, const void *data, u32 len)
{
    struct p445_header *h;
    struct p445_section s;
    u32 aligned, off, idx, i, words;
    const u32 *v = data;

    if (!p445_image || !data || !len || !name) return -EINVAL;
    aligned = ALIGN(len, 8U);
    off = (u32)atomic_add_return((int)aligned, &p445_used) - aligned;
    idx = (u32)atomic_inc_return(&p445_sections) - 1U;
    if (idx >= P445_MAX_SECTIONS || off > P445_IMAGE_BYTES ||
        aligned > P445_IMAGE_BYTES - off) {
        atomic_inc(&p445_dropped);
        p445_hdr()->flags |= P445_F_OVERFLOW;
        p445_hdr()->dropped_sections = (u32)atomic_read(&p445_dropped);
        p445_hdr()->section_capacity = P445_MAX_SECTIONS;
        pr_err("P445 DROP section idx=%u cap=%u off=%x len=%x dropped=%u\n",
            idx, P445_MAX_SECTIONS, off, aligned,
            (u32)atomic_read(&p445_dropped));
        p445_sync_header();
        return -ENOSPC;
    }

    memset(p445_image + off, 0, aligned);
    memcpy(p445_image + off, data, len);
    memset(&s, 0, sizeof(s));
    strlcpy(s.name, name, sizeof(s.name));
    s.stage = stage; s.type = type; s.offset = off; s.length = len;
    s.crc32 = p445_crc(data, len); s.aux0 = aux0; s.aux1 = aux1;
    words = len / sizeof(u32);
    for (i = 0; i < words; i++) {
        if (v[i] == 0) s.zero_count++;
        if (v[i] == ~0U) s.ffff_count++;
    }
    s.commit = P445_SEC_COMMIT;
    h = p445_hdr();
    memcpy(&h->sec[idx], &s, sizeof(s));
    wmb();
#if P445_KIND == 1
    if (p445_ram) {
        memcpy_toio((u8 __iomem *)p445_ram + off, p445_image + off, aligned);
        p445_flush_ram((u8 __iomem *)p445_ram + off, aligned);
    }
#endif
    p445_sync_header();
    return 0;
}
EXPORT_SYMBOL_GPL(a52_p445_store_section);

bool a52_p445_active(void)
{
    return atomic_read(&p445_state) == 1;
}
EXPORT_SYMBOL_GPL(a52_p445_active);

void a52_p445_try_arm(struct dsi_ctrl *ctrl, const struct mipi_dsi_msg *msg, u32 flags)
{
    const u8 *p;
    if (!ctrl || !msg || ctrl->cell_index != 0 ||
        flags != DSI_CTRL_CMD_FETCH_MEMORY || msg->type != 0x29 ||
        msg->tx_len != 3 || !msg->tx_buf) return;
    p = msg->tx_buf;
    if (p[0] != 0xf0 || p[1] != 0x5a || p[2] != 0x5a) return;
    if (atomic_cmpxchg(&p445_state, 0, 1) != 0) return;
    p445_ctrl = ctrl;
    atomic_set(&p445_rung, 0);
    atomic_set(&p445_gate, P445_GATE_OFF);
    p445_hdr()->rung = 0;
    p445_hdr()->gate = P445_GATE_OFF;
    memset(&p445_saved_msg, 0, sizeof(p445_saved_msg));
    p445_saved_msg.channel = msg->channel;
    p445_saved_msg.type = msg->type;
    p445_saved_msg.flags = msg->flags;
    p445_saved_msg.wait_ms = msg->wait_ms;
    memcpy(p445_saved_payload, p, sizeof(p445_saved_payload));
    p445_saved_msg.tx_buf = p445_saved_payload;
    p445_saved_msg.tx_len = sizeof(p445_saved_payload);
#if P445_KIND == 2
    pr_info("P445 ARM kind=%u flags=%x mf=%x boot=%llx\n",
        P445_KIND, flags, msg->flags,
        (unsigned long long)p445_hdr()->boot_id);
#endif
}
EXPORT_SYMBOL_GPL(a52_p445_try_arm);

/* A52_PHASE446J_DISPCC_MISC_READONLY_V1
 * DISP_CC_MISC_CMD is already mapped via ctrl->disp_cc_base.
 * Do not change reset or clock-gating bits.
 */
#define P446J_RING_N 128U
struct p446j_point { u64 ns; u32 tag, misc, a, b; } __packed;
struct p446j_dump {
    u32 total, kept;
    struct p446j_point point[P446J_RING_N];
} __packed;
static struct p446j_point p446j_ring[P446J_RING_N];
static struct p446j_dump p446j_dump;
static atomic_t p446j_seq = ATOMIC_INIT(0);
static atomic_t p446j_sealed = ATOMIC_INIT(0);

void a52_p446j_misc_event(struct dsi_ctrl_hw *ctrl, u32 tag, u32 a, u32 b)
{
    struct p446j_point *p;
    u32 seq;
    if (!ctrl || ctrl->index != 0 || atomic_read(&p446j_sealed)) return;
    seq=(u32)atomic_inc_return(&p446j_seq)-1U;
    p=&p446j_ring[seq % P446J_RING_N];
    p->ns=ktime_get_ns();
    p->tag=tag;
    p->misc=ctrl->disp_cc_base ? readl_relaxed(ctrl->disp_cc_base) : ~0U;
    p->a=a;
    p->b=b;
}
EXPORT_SYMBOL_GPL(a52_p446j_misc_event);

static void p446j_save(void)
{
    u32 total, n, i, first;
    atomic_set(&p446j_sealed,1);
    total=(u32)atomic_read(&p446j_seq);
    n=min(total,P446J_RING_N);
    first=total-n;
    p446j_dump.total=total;
    p446j_dump.kept=n;
    for(i=0;i<n;i++)
        p446j_dump.point[i]=p446j_ring[(first+i)%P446J_RING_N];
    a52_p445_store_section(0x446a0001U,14U,"J_MISC_TRACE",total,n,
        &p446j_dump,8U+n*sizeof(struct p446j_point));
}

static inline u32 p446i_r(void __iomem *b, u32 o)
{
    return b ? readl_relaxed((u8 __iomem *)b + o) : ~0U;
}

static int __init p446i_map_init(void)
{
    p446i_refgen=ioremap(P446I_REFGEN_PHYS,P446I_REFGEN_BYTES);
    p446i_dispcc=ioremap(P446I_DISPCC_PHYS,P446I_DISPCC_BYTES);
    p446i_mdp=ioremap(P446I_MDP_PHYS,P446I_MDP_BYTES);
    return 0;
}
late_initcall(p446i_map_init);

static bool p446i_exact_f0(struct dsi_ctrl *ctrl, const struct mipi_dsi_msg *msg)
{
    const u8 *p;
    if (!ctrl || !msg || ctrl->cell_index != 0 || msg->type != 0x29 ||
        msg->tx_len != 3 || !msg->tx_buf) return false;
    p=msg->tx_buf;
    return p[0]==0xf0 && p[1]==0x5a && p[2]==0x5a;
}

void a52_p446i_sw_entry(struct dsi_ctrl *ctrl, const struct mipi_dsi_msg *msg, u32 flags)
{
	/* A52_U2_AB_OBSERVER_QUIET_V1 */
	static bool a52_u2_ab_disable_a52_p446i_sw_entry = true;
	if (!READ_ONCE(a52_u2_ab_disable_a52_p446i_sw_entry)) {

    struct p446i_sw *m=&p446i_sw;
    if (!p446i_exact_f0(ctrl,msg)) return;
    if (atomic_cmpxchg(&p446i_seen,0,1) != 0) return;
    memset(m,0,sizeof(*m));
    m->flags=flags; m->msg_flags=(u32)msg->flags;
    m->panel_mode=(u32)ctrl->host_config.panel_mode;
    m->power_state=(u32)ctrl->current_state.power_state;
    m->host_initialized=(u32)ctrl->current_state.host_initialized;
    m->controller_state=(u32)ctrl->current_state.controller_state;
    m->cmd_engine_state=(u32)ctrl->current_state.cmd_engine_state;
    m->vid_engine_state=(u32)ctrl->current_state.vid_engine_state;
    m->byte_rate=ctrl->clk_freq.byte_clk_rate;
    m->pix_rate=ctrl->clk_freq.pix_clk_rate;
    m->byte_intf_rate=ctrl->clk_freq.byte_intf_clk_rate;
    m->esc_rate=ctrl->clk_freq.esc_clk_rate;
    atomic_set(&p446i_target,1);
    a52_p445_store_section(P446I_STAGE_SW,P446I_TYPE_SW,
        "I_SW_ENTRY",flags,(u32)msg->flags,m,sizeof(*m));

	}
	return;
}
EXPORT_SYMBOL_GPL(a52_p446i_sw_entry);

static void p446i_fill_raw(struct p446i_raw *r, struct dsi_ctrl_hw *ctrl)
{
    u32 i,j,saved;
    static const u32 mdp_off[12] = {
        0x1008U,0x100cU,0x1010U,0x1014U,
        0x6b800U,0x6b8a8U,0x6b8acU,0x6b8b0U,
        0x71014U,0x71028U,0x7102cU,0x71030U
    };
    memset(r,0,sizeof(*r)); r->ns=ktime_get_ns();
    if (ctrl && ctrl->base) {
        r->dsi_status=DSI_R32(ctrl,DSI_STATUS);
        r->lane_status=DSI_R32(ctrl,DSI_LANE_STATUS);
        r->clk_status=DSI_R32(ctrl,DSI_CLK_STATUS);
        r->int_ctrl=DSI_R32(ctrl,DSI_INT_CTRL);
        saved=DSI_R32(ctrl,DSI_DEBUG_BUS_CTL); r->dbg_ctl=saved;
        for(i=0;i<ARRAY_SIZE(p446i_compact_sel);i++){
            DSI_W32(ctrl,DSI_DEBUG_BUS_CTL,p446i_compact_sel[i]); wmb();
            r->compact[i]=DSI_R32(ctrl,DSI_DEBUG_BUS_STATUS);
        }
        DSI_W32(ctrl,DSI_DEBUG_BUS_CTL,saved); wmb();
    }
    for(i=0;i<64;i++) r->refgen[i]=p446i_r(p446i_refgen,i*4U);
    for(i=0;i<ARRAY_SIZE(p446i_rcg_base);i++)
        for(j=0;j<5;j++) r->rcg[i*5+j]=p446i_r(p446i_dispcc,p446i_rcg_base[i]+j*4U);
    for(i=0;i<ARRAY_SIZE(p446i_pll_off);i++) r->pll0[i]=p446i_r(p446i_dispcc,p446i_pll_off[i]);
    for(i=0;i<ARRAY_SIZE(mdp_off);i++) r->mdp[i]=p446i_r(p446i_mdp,mdp_off[i]);
}

static void p446i_sweep(struct p446i_bus_pair *out, struct dsi_ctrl_hw *ctrl)
{
    u32 i,saved;
    if (!ctrl || !ctrl->base) return;
    saved=DSI_R32(ctrl,DSI_DEBUG_BUS_CTL);
    for(i=0;i<ARRAY_SIZE(p446i_sel);i++){
        out[i].selector=p446i_sel[i];
        DSI_W32(ctrl,DSI_DEBUG_BUS_CTL,p446i_sel[i]); wmb();
        out[i].value=DSI_R32(ctrl,DSI_DEBUG_BUS_STATUS);
    }
    DSI_W32(ctrl,DSI_DEBUG_BUS_CTL,saved); wmb();
}


/* A52_PHASE446K_COLD_PAIR_V1: full CTRL 0..3fc / PHY 0..ffc */
#define KCTRL 256U
#define KPHY 1024U
struct p446k_capture {
    u64 ns;
    u32 tag,misc;
    u32 ctrl[KCTRL],phy[KPHY];
    struct p446i_raw aux;
    struct p446i_bus_pair bus[ARRAY_SIZE(p446i_sel)];
} __packed;
static struct p446k_capture p446k_pre,p446k_on;
static void __iomem *p446k_phy;
static atomic_t p446k_pre_once=ATOMIC_INIT(0),p446k_on_once=ATOMIC_INIT(0);
static int __init p446k_map_init(void)
{
    p446k_phy=ioremap(0x0ae94000ULL,0x1000U);
    if (!p446k_phy) pr_warn("P446K PHY MMIO mapping failed\n");
    return 0;
}
late_initcall(p446k_map_init);
static u32 p446k_hash(const u32 *p,u32 n)
{
    u32 i,h=2166136261U;
    for(i=0;i<n;i++) h=(h^p[i])*16777619U;
    return h;
}
void a52_p446k_capture(struct dsi_ctrl_hw *ctrl,u32 tag)
{
    u32 i,hc,hp;
    struct p446k_capture *s;
    if (!ctrl || ctrl->index!=0 || !ctrl->base) return;
    if (tag==1U) {
        if (atomic_cmpxchg(&p446k_pre_once,0,1)!=0) return;
        s=&p446k_pre;
    } else if(tag==2U) {
        if (atomic_read(&p446k_pre_once)!=1 ||
            atomic_cmpxchg(&p446k_on_once,0,1)!=0) return;
        s=&p446k_on;
    } else return;
    s->ns=ktime_get_ns();
    s->tag=tag;
    s->misc=ctrl->disp_cc_base ? readl_relaxed(ctrl->disp_cc_base) : ~0U;
    for(i=0;i<KCTRL;i++) s->ctrl[i]=DSI_R32(ctrl,i*4U);
    for(i=0;i<KPHY;i++) s->phy[i]=p446i_r(p446k_phy,i*4U);
    p446i_fill_raw(&s->aux,ctrl);
    p446i_sweep(s->bus,ctrl);
    hc=p446k_hash(s->ctrl,KCTRL);
    hp=p446k_hash(s->phy,KPHY);
    pr_warn("P446K tag=%u ns=%llu misc=%08x refgen80=%08x clk=%08x lane=%08x db171=%08x CTRL=%08x PHY=%08x\n",
       tag,(unsigned long long)s->ns,s->misc,s->aux.refgen[32],
       s->aux.clk_status,s->aux.lane_status,s->aux.compact[1],hc,hp);
    if (tag==1U) {
        a52_p445_store_section(0x446b0011U,15U,"K_PRE_CTRL",tag,hc,s->ctrl,sizeof(s->ctrl));
        a52_p445_store_section(0x446b0012U,15U,"K_PRE_PHY",tag,hp,s->phy,sizeof(s->phy));
        a52_p445_store_section(0x446b0013U,15U,"K_PRE_AUX",tag,s->misc,&s->aux,sizeof(s->aux));
        a52_p445_store_section(0x446b0014U,15U,"K_PRE_BUS",tag,ARRAY_SIZE(p446i_sel),s->bus,sizeof(s->bus));
    } else {
        a52_p445_store_section(0x446b0021U,15U,"K_ON_CTRL",tag,hc,s->ctrl,sizeof(s->ctrl));
        a52_p445_store_section(0x446b0022U,15U,"K_ON_PHY",tag,hp,s->phy,sizeof(s->phy));
        a52_p445_store_section(0x446b0023U,15U,"K_ON_AUX",tag,s->misc,&s->aux,sizeof(s->aux));
        a52_p445_store_section(0x446b0024U,15U,"K_ON_BUS",tag,ARRAY_SIZE(p446i_sel),s->bus,sizeof(s->bus));
    }
}
EXPORT_SYMBOL_GPL(a52_p446k_capture);

/* A52_PHASE446L_FIRST_COLD_ON_TRIGGER_V1 */
#define P446L_WORDS 256U
#define P446L_DISPCC_WORDS 128U
struct p446l_meta {
    u64 iova, phys;
    u32 len, flags, msg_flags, domain_found, prefix_len;
    u8 prefix[16];
} __packed;
struct p446l_sample {
    u64 ns_begin, ns_end;
    u32 tag, hw_flags, elapsed_us, status, lane, clk, int_ctrl;
    u32 ctrl[P446L_WORDS];
    u32 dispcc[P446L_DISPCC_WORDS]; /* offsets 0x1000..0x11fc */
    u32 gdsc;  /* real MDSS GDSC @0x0af01004 */
    u32 cal_l; /* actual Fabia CAL_L readback */
    u32 gcc[6]; /* 1b7040/44 votes, 1700c/1c/70/34 CBCRs */
    u32 hf0_tbu; /* 0x15182210 */
    u32 tcs[2][2][4+16*4]; /* each: ctl/st/en/wait + 16 CMD message/address/data/status */
    u32 cb2_route,cb2_fsr,cb2_fsynr0,cb2_sctlr;
    u64 cb2_far,cb2_ttbr0;
    struct p446i_bus_pair bus[ARRAY_SIZE(p446i_sel)];
}; /* live CB2 fields are passed by address; retain native alignment */
static struct p446l_meta p446l_cmd;
static struct p446l_sample p446l_t3,p446l_t4;
struct p446l_fast {
    u64 ns; u32 deadline_us,status,irq,lane,clk;
} __packed;
static struct p446l_fast p446l_fast[7];
static const u32 p446l_deadline_us[7]={0U,1U,2U,5U,10U,20U,50U};
static void __iomem *p446l_votes,*p446l_gcc,*p446l_gdsc,*p446l_hf0;
static void __iomem *p446l_disp_rsc,*p446l_apps_rsc;
static int __init p446l_map_init(void)
{
    p446l_votes=ioremap(0x001b7000ULL,0x1000);
    p446l_gcc=ioremap(0x00117000ULL,0x1000);
    p446l_gdsc=ioremap(0x0af01000ULL,0x1000);
    p446l_hf0=ioremap(0x15182000ULL,0x1000);
    p446l_disp_rsc=ioremap(0x0af20000ULL,0x3000);
    p446l_apps_rsc=ioremap(0x18220000ULL,0x3000);
    return 0;
}
late_initcall(p446l_map_init);
static void p446l_take_fast(struct dsi_ctrl_hw *hw, u64 start)
{
    u32 i;
    for (i=0;i<ARRAY_SIZE(p446l_fast);i++) {
        struct p446l_fast *f=&p446l_fast[i];
        while (ktime_get_ns()-start <
               (u64)p446l_deadline_us[i]*1000ULL)
            cpu_relax();
        f->ns=ktime_get_ns();
        f->deadline_us=p446l_deadline_us[i];
        f->status=DSI_R32(hw,DSI_STATUS);
        f->irq=DSI_R32(hw,DSI_INT_CTRL);
        f->lane=DSI_R32(hw,DSI_LANE_STATUS);
        f->clk=DSI_R32(hw,DSI_CLK_STATUS);
    }
}
static void p446l_take_tcs(struct p446l_sample *s)
{
    void __iomem *base[2]={p446l_disp_rsc,p446l_apps_rsc};
    const u32 off[2]={0x1c00U,0x0d00U};
    u32 i,t,j;
    for (i=0;i<2;i++) for(t=0;t<2;t++) {
        u32 o=off[i]+t*672U;
        s->tcs[i][t][0]=p446i_r(base[i],o+0x14U);
        s->tcs[i][t][1]=p446i_r(base[i],o+0x18U);
        s->tcs[i][t][2]=p446i_r(base[i],o+0x1cU);
        s->tcs[i][t][3]=p446i_r(base[i],o+0x10U);
        for(j=0;j<16U;j++) {
            u32 cmd=o+j*20U;
            s->tcs[i][t][4+4*j+0]=p446i_r(base[i],cmd+0x30U);
            s->tcs[i][t][4+4*j+1]=p446i_r(base[i],cmd+0x34U);
            s->tcs[i][t][4+4*j+2]=p446i_r(base[i],cmd+0x38U);
            s->tcs[i][t][4+4*j+3]=p446i_r(base[i],cmd+0x3cU);
        }
    }
}

static atomic_t p446l_state=ATOMIC_INIT(0); /* 0 idle; 1 armed; 2 taken */

void a52_p446l_arm(struct dsi_ctrl_hw *ctrl)
{
    if (!ctrl || ctrl->index!=0 || !ctrl->base)
        return;
    (void)atomic_cmpxchg(&p446l_state,0,1);
}
EXPORT_SYMBOL_GPL(a52_p446l_arm);

bool a52_p446l_is_armed(void)
{
    return atomic_read(&p446l_state)==1;
}
EXPORT_SYMBOL_GPL(a52_p446l_is_armed);
void a52_p446l_note(struct dsi_ctrl_hw *ctrl, u64 iova, u64 phys,
                    u32 len, u32 flags, u32 msg_flags,
                    u32 domain_found, const u8 *prefix)
{
    if (!ctrl || ctrl->index!=0 || atomic_read(&p446l_state)!=1)
        return;
    p446l_cmd.iova=iova;
    p446l_cmd.phys=phys;
    p446l_cmd.len=len;
    p446l_cmd.flags=flags;
    p446l_cmd.msg_flags=msg_flags;
    p446l_cmd.domain_found=domain_found;
    p446l_cmd.prefix_len=min(len,16U);
    memset(p446l_cmd.prefix,0,sizeof(p446l_cmd.prefix));
    if (prefix && p446l_cmd.prefix_len)
        memcpy(p446l_cmd.prefix,prefix,p446l_cmd.prefix_len);
}
EXPORT_SYMBOL_GPL(a52_p446l_note);

extern void a52_p446l_cb2_live(u32 *,u32 *,u64 *,u32 *,u32 *,u64 *);
static void p446l_take(struct dsi_ctrl_hw *hw,
                        struct p446l_sample *s,u32 tag,u32 flags)
{
    u32 i;
    s->ns_begin=ktime_get_ns();
    s->tag=tag;
    s->hw_flags=flags;
    s->status=DSI_R32(hw,DSI_STATUS);
    s->lane=DSI_R32(hw,DSI_LANE_STATUS);
    s->clk=DSI_R32(hw,DSI_CLK_STATUS);
    s->int_ctrl=DSI_R32(hw,DSI_INT_CTRL);
    for(i=0;i<P446L_WORDS;i++)
        s->ctrl[i]=DSI_R32(hw,i*4U);
    for(i=0;i<P446L_DISPCC_WORDS;i++)
        s->dispcc[i]=p446i_r(p446i_dispcc,0x1000U+i*4U);
    s->gdsc=p446i_r(p446l_gdsc,0x004U);
    s->cal_l=p446i_r(p446i_dispcc,0x008U);
    s->gcc[0]=p446i_r(p446l_votes,0x040U); /* HF0 vote GDSC */
    s->gcc[1]=p446i_r(p446l_votes,0x044U); /* SF vote GDSC */
    s->gcc[2]=p446i_r(p446l_gcc,0x00cU); /* disp AHB */
    s->gcc[3]=p446i_r(p446l_gcc,0x01cU); /* disp AXI */
    s->gcc[4]=p446i_r(p446l_gcc,0x070U); /* CC XO */
    s->gcc[5]=p446i_r(p446l_gcc,0x034U); /* disp XO */
    s->hf0_tbu=p446i_r(p446l_hf0,0x210U);
    p446l_take_tcs(s);
    s->cb2_route=~0U;
    s->cb2_fsr=~0U;
    s->cb2_fsynr0=~0U;
    s->cb2_sctlr=~0U;
    s->cb2_far=~0ULL;
    s->cb2_ttbr0=~0ULL;
    a52_p446l_cb2_live(&s->cb2_route,&s->cb2_fsr,&s->cb2_far,
                        &s->cb2_fsynr0,&s->cb2_sctlr,&s->cb2_ttbr0);
    p446i_sweep(s->bus,hw);
    s->ns_end=ktime_get_ns();
    s->elapsed_us=(u32)((s->ns_end-s->ns_begin)/1000ULL);
}

void a52_p446l_after_trigger(struct dsi_ctrl_hw *hw,u32 hw_flags)
{
    u64 trigger_ns;
    u32 c3,c4;
    if (!hw || hw->index!=0 || !hw->base ||
        atomic_cmpxchg(&p446l_state,1,2)!=1)
        return;
    trigger_ns=ktime_get_ns();
    p446l_take_fast(hw,trigger_ns);
    p446l_take(hw,&p446l_t3,3U,hw_flags);
    /* Approx. +200 us from trigger. If t3 itself costs >200 us,
     * retain the measured timestamp instead of pretending timing matched.
     */
    while(ktime_get_ns()-trigger_ns<200000ULL)
        cpu_relax();
    p446l_take(hw,&p446l_t4,4U,hw_flags);
    c3=p446k_hash(p446l_t3.ctrl,P446L_WORDS);
    c4=p446k_hash(p446l_t4.ctrl,P446L_WORDS);
    a52_p445_store_section(0x446c0030U,16U,"L_CMD_META",p446l_cmd.flags,
        p446l_cmd.domain_found,&p446l_cmd,sizeof(p446l_cmd));
    a52_p445_store_section(0x446c0035U,16U,"L_FAST_0_50",7U,50U,
        p446l_fast,sizeof(p446l_fast));
    a52_p445_store_section(0x446c0031U,16U,"L_T3_CTRL",3U,c3,
        p446l_t3.ctrl,sizeof(p446l_t3.ctrl));
    a52_p445_store_section(0x446c0032U,16U,"L_T3_CLK",3U,p446l_t3.elapsed_us,
        p446l_t3.dispcc,sizeof(p446l_t3.dispcc));
    a52_p445_store_section(0x446c0033U,16U,"L_T3_BUS",3U,ARRAY_SIZE(p446i_sel),
        p446l_t3.bus,sizeof(p446l_t3.bus));
    a52_p445_store_section(0x446c0034U,16U,"L_T3_META",3U,p446l_t3.elapsed_us,
        &p446l_t3,offsetof(struct p446l_sample,ctrl));
    a52_p445_store_section(0x446c0036U,16U,"L_T3_PLATFORM",3U,p446l_t3.cal_l,
        &p446l_t3.gdsc,offsetof(struct p446l_sample,bus)-
        offsetof(struct p446l_sample,gdsc));
    a52_p445_store_section(0x446c0041U,16U,"L_T4_CTRL",4U,c4,
        p446l_t4.ctrl,sizeof(p446l_t4.ctrl));
    a52_p445_store_section(0x446c0042U,16U,"L_T4_CLK",4U,p446l_t4.elapsed_us,
        p446l_t4.dispcc,sizeof(p446l_t4.dispcc));
    a52_p445_store_section(0x446c0043U,16U,"L_T4_BUS",4U,ARRAY_SIZE(p446i_sel),
        p446l_t4.bus,sizeof(p446l_t4.bus));
    a52_p445_store_section(0x446c0044U,16U,"L_T4_META",4U,p446l_t4.elapsed_us,
        &p446l_t4,offsetof(struct p446l_sample,ctrl));
    a52_p445_store_section(0x446c0046U,16U,"L_T4_PLATFORM",4U,p446l_t4.cal_l,
        &p446l_t4.gdsc,offsetof(struct p446l_sample,bus)-
        offsetof(struct p446l_sample,gdsc));
    pr_warn("P446L FAST 0us st=%08x irq=%08x lane=%08x clk=%08x; 5us st=%08x irq=%08x lane=%08x clk=%08x; 50us st=%08x irq=%08x lane=%08x clk=%08x\n",
        p446l_fast[0].status,p446l_fast[0].irq,p446l_fast[0].lane,p446l_fast[0].clk,
        p446l_fast[3].status,p446l_fast[3].irq,p446l_fast[3].lane,p446l_fast[3].clk,
        p446l_fast[6].status,p446l_fast[6].irq,p446l_fast[6].lane,p446l_fast[6].clk);
    pr_warn("P446L PLATFORM cal=%08x/%08x gdsc=%08x/%08x hf_vote=%08x/%08x sf_vote=%08x/%08x hf0=%08x/%08x gcc_ahb=%08x/%08x gcc_axi=%08x/%08x\n",
        p446l_t3.cal_l,p446l_t4.cal_l,p446l_t3.gdsc,p446l_t4.gdsc,
        p446l_t3.gcc[0],p446l_t4.gcc[0],p446l_t3.gcc[1],p446l_t4.gcc[1],
        p446l_t3.hf0_tbu,p446l_t4.hf0_tbu,p446l_t3.gcc[2],p446l_t4.gcc[2],
        p446l_t3.gcc[3],p446l_t4.gcc[3]);
    pr_warn("P446L CB2 t3 route=%08x fsr=%08x fsynr=%08x far=%llx sctlr=%08x ttbr=%llx t4 fsr=%08x fsynr=%08x\n",
        p446l_t3.cb2_route,p446l_t3.cb2_fsr,p446l_t3.cb2_fsynr0,
        (unsigned long long)p446l_t3.cb2_far,p446l_t3.cb2_sctlr,
        (unsigned long long)p446l_t3.cb2_ttbr0,
        p446l_t4.cb2_fsr,p446l_t4.cb2_fsynr0);
    pr_warn("P446L post-SW_TRIGGER +%lluus t3=%llu..%llu t4=%llu..%llu st=%08x/%08x clk=%08x/%08x lane=%08x/%08x int=%08x/%08x CTRL=%08x/%08x IOVA=%llx PHYS=%llx valid=%u len=%u flags=%08x\n",
        (unsigned long long)((p446l_t4.ns_begin-trigger_ns)/1000ULL),
        (unsigned long long)p446l_t3.ns_begin,
        (unsigned long long)p446l_t3.ns_end,
        (unsigned long long)p446l_t4.ns_begin,
        (unsigned long long)p446l_t4.ns_end,
        p446l_t3.status,p446l_t4.status,
        p446l_t3.clk,p446l_t4.clk,
        p446l_t3.lane,p446l_t4.lane,
        p446l_t3.int_ctrl,p446l_t4.int_ctrl,c3,c4,
        (unsigned long long)p446l_cmd.iova,
        (unsigned long long)p446l_cmd.phys,
        p446l_cmd.domain_found,p446l_cmd.len,p446l_cmd.flags);
}
EXPORT_SYMBOL_GPL(a52_p446l_after_trigger);

void a52_p446i_hw_pre(struct dsi_ctrl_hw *ctrl)
{
	/* A52_U2_AB_OBSERVER_QUIET_V1 */
	static bool a52_u2_ab_disable_a52_p446i_hw_pre = true;
	if (!READ_ONCE(a52_u2_ab_disable_a52_p446i_hw_pre)) {

    if (!atomic_read(&p446i_target)) return;
    a52_p446j_misc_event(ctrl,0x4a01U,0,0);
    p446i_fill_raw(&p446i_pre,ctrl);
    p446i_sweep(p446i_bus_pre,ctrl);

	}
	return;
}
EXPORT_SYMBOL_GPL(a52_p446i_hw_pre);

void a52_p446i_hw_post(struct dsi_ctrl_hw *ctrl)
{
	/* A52_U2_AB_OBSERVER_QUIET_V1 */
	static bool a52_u2_ab_disable_a52_p446i_hw_post = true;
	if (!READ_ONCE(a52_u2_ab_disable_a52_p446i_hw_post)) {

    if (!atomic_read(&p446i_target)) return;
    a52_p446j_misc_event(ctrl,0x4a02U,0,0);
    p446i_fill_raw(&p446i_post,ctrl);
    p446i_sweep(p446i_bus_post,ctrl);
    a52_p445_store_section(P446I_STAGE_PRE,P446I_TYPE_RAW,"I_RAW_PRE",
        (u32)p446i_pre.refgen[0x80/4],p446i_pre.compact[1],&p446i_pre,sizeof(p446i_pre));
    a52_p445_store_section(P446I_STAGE_POST,P446I_TYPE_RAW,"I_RAW_POST",
        (u32)p446i_post.refgen[0x80/4],p446i_post.compact[1],&p446i_post,sizeof(p446i_post));
    a52_p445_store_section(P446I_STAGE_PRE,P446I_TYPE_BUS,"I_BUS_PRE",
        ARRAY_SIZE(p446i_sel),p446i_pre.dbg_ctl,p446i_bus_pre,sizeof(p446i_bus_pre));
    a52_p445_store_section(P446I_STAGE_POST,P446I_TYPE_BUS,"I_BUS_POST",
        ARRAY_SIZE(p446i_sel),p446i_post.dbg_ctl,p446i_bus_post,sizeof(p446i_bus_post));
    p446j_save();
    atomic_set(&p446i_target,0);

	}
	return;
}
EXPORT_SYMBOL_GPL(a52_p446i_hw_post);

static void p445_hot_one(struct dsi_ctrl_hw *ctrl, u32 point, u64 base)
{
    struct p445_hot *s;
    int i = atomic_inc_return(&p445_hot_count) - 1;
    if (i < 0 || i >= P445_MAX_HOT) return;
    s = &p445_hot[i];
    memset(s, 0, sizeof(*s));
    s->dt_ns = ktime_get_ns() - base;
    s->point = point;
    s->status = DSI_R32(ctrl, DSI_STATUS);
    s->clk_status = DSI_R32(ctrl, DSI_CLK_STATUS);
    s->int_ctrl = DSI_R32(ctrl, DSI_INT_CTRL);
    s->lane_status = DSI_R32(ctrl, DSI_LANE_STATUS);
    s->dbg171 = DSI_R32(ctrl, DSI_DEBUG_BUS_STATUS);
    s->crc32 = p445_crc(s, offsetof(struct p445_hot, crc32));
}

void a52_p445_pre_trigger(struct dsi_ctrl_hw *ctrl)
{
    u64 now;
    if (!a52_p445_active() || !ctrl || !ctrl->base) return;
    p445_saved_dbg = DSI_R32(ctrl, DSI_DEBUG_BUS_CTL);
    DSI_W32(ctrl, DSI_DEBUG_BUS_CTL, 0x0171);
    wmb();
    atomic_set(&p445_hot_count, 0);
    now = ktime_get_ns();
    p445_hdr()->pretrigger_ns = now;
    p445_hdr()->trigger_ns = now; /* timestamp immediately before SW_TRIGGER */
    p445_hot_one(ctrl, ~0U, now);
}
EXPORT_SYMBOL_GPL(a52_p445_pre_trigger);

void a52_p445_post_trigger(struct dsi_ctrl_hw *ctrl)
{
    u64 base, target;
    u32 i;
    if (!a52_p445_active() || !ctrl || !ctrl->base) return;
    base = READ_ONCE(p445_hdr()->trigger_ns);
    for (i = 0; i <= 25U; i++) {
        target = base + (u64)i * 3000ULL;
        while (ktime_get_ns() < target) cpu_relax();
        p445_hot_one(ctrl, i * 3U, base);
    }
    DSI_W32(ctrl, DSI_DEBUG_BUS_CTL, p445_saved_dbg);
    wmb();
    {
        u32 rung=(u32)atomic_read(&p445_rung);
        u32 stage = rung == 0 ? P445_STAGE_R0_HOT :
                    (rung == 1 ? P445_STAGE_R1_HOT : P445_STAGE_R2_HOT);
        const char *name = rung == 0 ? "R0_HOT_0_75US" :
                           (rung == 1 ? "R1_HOT_0_75US" : "R2_HOT_0_75US");
        a52_p445_store_section(stage, P445_TYPE_HOT, name,
            3000U, (u32)atomic_read(&p445_hot_count), p445_hot,
            (u32)atomic_read(&p445_hot_count) * sizeof(p445_hot[0]));
    }
}
EXPORT_SYMBOL_GPL(a52_p445_post_trigger);

static void p445_direct_dsi(u32 stage, struct dsi_ctrl_hw *ctrl)
{
    static const u32 off[] = {
        DSI_CTRL, DSI_STATUS, DSI_FIFO_STATUS, DSI_COMMAND_MODE_DMA_CTRL,
        DSI_DMA_CMD_OFFSET, DSI_DMA_CMD_LENGTH, DSI_TRIG_CTRL,
        DSI_LANE_STATUS, DSI_LANE_CTRL, DSI_ACK_ERR_STATUS,
        DSI_DLN0_PHY_ERR, DSI_TIMEOUT_STATUS, DSI_INT_CTRL,
        DSI_CLK_CTRL, DSI_CLK_STATUS, DSI_DEBUG_BUS_CTL,
        DSI_DEBUG_BUS_STATUS, DSI_AXI2AHB_CTRL, DSI_TEST_PATTERN_GEN_CTRL
    };
    struct p445_pair p[ARRAY_SIZE(off)];
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(off); i++) { p[i].key = off[i]; p[i].value = DSI_R32(ctrl, off[i]); }
    a52_p445_store_section(stage, P445_TYPE_DSI_DIRECT, "DSI_DIRECT", 0, 0, p, sizeof(p));
}

static void p445_phy_status(u32 stage)
{
    static const u32 off[] = { 0x414, 0x41c, 0x424, 0x438, 0x4f4, 0x4f8, 0xba0 };
    struct p445_pair p[ARRAY_SIZE(off)];
    unsigned int i;
    if (!p445_phy) return;
    for (i = 0; i < ARRAY_SIZE(off); i++) { p[i].key = off[i]; p[i].value = readl_relaxed((u8 __iomem *)p445_phy + off[i]); }
    a52_p445_store_section(stage, P445_TYPE_PHY, "DSI_PHY_SAFE", 0xae94000U, 0, p, sizeof(p));
}

static void __maybe_unused p445_deep(u32 stage, struct dsi_ctrl *ctrl)
{
    struct dsi_clk_ctrl_info ci;
    int clk_rc = -ENODEV;
    if (!ctrl || !ctrl->hw.base) return;
    a52_p445_frontier(stage, 1U);
    memset(&ci, 0, sizeof(ci));
    ci.client = DSI_CLK_REQ_DSI_CLIENT;
    ci.clk_type = DSI_CORE_CLK;
    ci.clk_state = DSI_CLK_ON;
    if (ctrl->clk_cb.dsi_clk_cb)
        clk_rc = ctrl->clk_cb.dsi_clk_cb(ctrl->clk_cb.priv, ci);
    a52_p445_frontier(stage, 2U);
    p445_direct_dsi(stage, &ctrl->hw);
    p445_phy_status(stage);
    a52_p445_frontier(stage, 3U);
    a52_p445_sde_capture(stage, &ctrl->hw);
    a52_p445_frontier(stage, 4U);
    if (!clk_rc && ctrl->clk_cb.dsi_clk_cb) {
        ci.clk_state = DSI_CLK_OFF;
        ctrl->clk_cb.dsi_clk_cb(ctrl->clk_cb.priv, ci);
    }
}

static void p445_checkpoint(void);

#if P445_KIND == 1 && P445_DISK_ENABLED
static int p445_submit_page(struct block_device *bdev, void *addr, unsigned int page_index)
{
    struct bio *bio; struct page *page; int added, rc;
    page = vmalloc_to_page(addr); if (!page) return -EFAULT;
    bio = bio_alloc(GFP_KERNEL, 1); if (!bio) return -ENOMEM;
    bio_set_dev(bio, bdev);
    bio->bi_iter.bi_sector = (sector_t)((P445_DEBUG_OFFSET + (u64)page_index * PAGE_SIZE) >> 9);
    bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_SYNC | REQ_FUA);
    added = bio_add_page(bio, page, PAGE_SIZE, 0);
    if (added != PAGE_SIZE) { bio_put(bio); return -EIO; }
    rc = submit_bio_wait(bio); bio_put(bio); return rc;
}
static void p445_disk_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(p445_disk_work, p445_disk_workfn);
static void p445_checkpoint(void)
{
    /* P445 v1: disk tier disabled; P414 already owns 8-10 MiB of debug. */
    p445_sync_header();
}
static void p445_disk_workfn(struct work_struct *work)
{
    struct block_device *bdev; unsigned int i, pages, gen; int rc=0, frc=0, retry;
    (void)work; if (!p445_image) return;
    gen = (u32)atomic_read(&p445_disk_gen);
    pages = DIV_ROUND_UP((u32)atomic_read(&p445_used), PAGE_SIZE);
    bdev = blkdev_get_by_dev(MKDEV(SCSI_DISK0_MAJOR, 8), FMODE_WRITE, NULL);
    if (IS_ERR(bdev)) { rc = PTR_ERR(bdev); goto retry_out; }
    /* Commit data pages first and header page last. A torn write can then
     * never expose a new section table pointing at old payload pages. */
    for (i=1; i<pages; i++) { rc=p445_submit_page(bdev,p445_image+i*PAGE_SIZE,i); if(rc) break; }
    if (!rc) rc=p445_submit_page(bdev,p445_image,0);
    if (!rc) frc=blkdev_issue_flush(bdev,GFP_KERNEL);
    blkdev_put(bdev,FMODE_WRITE);
retry_out:
    p445_hdr()->disk_submit_rc=rc; p445_hdr()->disk_flush_rc=frc;
    if (!rc && !frc) {
        atomic_set(&p445_disk_written, gen); atomic_set(&p445_disk_retry,0);
        p445_hdr()->flags |= P445_F_DISK_OK; p445_sync_header();
        if ((u32)atomic_read(&p445_disk_gen)!=gen) mod_delayed_work(system_unbound_wq,&p445_disk_work,0);
        return;
    }
    retry=atomic_inc_return(&p445_disk_retry); p445_sync_header();
    if (retry<=240) mod_delayed_work(system_unbound_wq,&p445_disk_work,msecs_to_jiffies(250));
}
#else
static void p445_checkpoint(void) { p445_sync_header(); }
#endif


struct p445_power_meta {
    u32 tag, gate, rung;
    u32 panel_initialized, ulps_feature, ulps_suspend, samsung_state;
    u32 panel_supply_refcount;
    u32 ctrl_digital_refcount, ctrl_host_refcount;
    u32 phy_digital_refcount, phy_pwr_refcount;
    u32 core_ref, link_ref, core_state, link_state;
    u32 pll_handoff, pll_on, pll_prepare, pll_enable, pll_lock, pll_resource_ref;
    u32 refgen_registered, refgen_enabled, refgen_use_count, refgen_pwrdwn;
} __packed;

#define P445_SUPPLY_PANEL 1U
#define P445_SUPPLY_CTRL_DIGITAL 2U
#define P445_SUPPLY_CTRL_HOST 3U
#define P445_SUPPLY_PHY_DIGITAL 4U
#define P445_SUPPLY_PHY_PWR 5U
struct p445_supply_rec {
    char name[32];
    u32 group, group_refcount;
    s32 enabled, use_count, voltage;
    u32 min_uv, max_uv;
} __packed;

struct p445_devrec {
    char name[40];
    char driver[32];
    char compat[48];
} __packed;

struct p445_bound_ctx {
    struct p445_devrec *r;
    u32 n, max;
};

static int p445_bound_one(struct device *dev, void *data)
{
    struct p445_bound_ctx *c=data;
    struct p445_devrec *r;
    const char *compat=NULL;
    if (!c || !dev || !dev->driver || c->n >= c->max) return 0;
    r=&c->r[c->n++];
    memset(r,0,sizeof(*r));
    strlcpy(r->name,dev_name(dev),sizeof(r->name));
    if (dev->driver && dev->driver->name)
        strlcpy(r->driver,dev->driver->name,sizeof(r->driver));
    if (dev->of_node)
        compat=of_get_property(dev->of_node,"compatible",NULL);
    if (compat) strlcpy(r->compat,compat,sizeof(r->compat));
    return 0;
}

static void p445_mmio_range(u32 stage, const char *name,
        void __iomem *base, u32 bytes)
{
    static u32 scratch[0x800U / sizeof(u32)];
    u32 i, words;
    if (!base || !bytes || bytes > sizeof(scratch)) return;
    words=bytes/sizeof(u32);
    for(i=0;i<words;i++) scratch[i]=readl_relaxed((u8 __iomem *)base+i*4U);
    a52_p445_store_section(stage,P445_TYPE_SDE_REG,name,0,bytes,scratch,bytes);
}

static void p445_selectors(u32 stage, struct dsi_ctrl_hw *ctrl, const char *name)
{
    static const u32 sel[]={0x151,0x171,0x181,0x191,0x1a1,0x1b1,0x1c1,0x211};
    struct p445_pair p[ARRAY_SIZE(sel)];
    u32 saved,i;
    if (!ctrl || !ctrl->base) return;
    saved=DSI_R32(ctrl,DSI_DEBUG_BUS_CTL);
    for(i=0;i<ARRAY_SIZE(sel);i++) {
        DSI_W32(ctrl,DSI_DEBUG_BUS_CTL,sel[i]);
        wmb();
        p[i].key=sel[i];
        p[i].value=DSI_R32(ctrl,DSI_DEBUG_BUS_STATUS);
    }
    DSI_W32(ctrl,DSI_DEBUG_BUS_CTL,saved);
    wmb();
    a52_p445_store_section(stage,P445_TYPE_DSI_BUS,name,0,0,p,sizeof(p));
}

static void p445_compact(u32 stage, struct dsi_ctrl *ctrl, const char *tag)
{
    char n[24];
    if (!ctrl || !ctrl->hw.base) return;
    if (!p445_phy) p445_phy=ioremap(0x0ae94000ULL,0x1000U);
    snprintf(n,sizeof(n),"%s_DSI",tag);
    p445_direct_dsi(stage,&ctrl->hw);
    snprintf(n,sizeof(n),"%s_CTRL",tag);
    p445_mmio_range(stage,n,ctrl->hw.base,0x400U);
    if (p445_phy) {
        snprintf(n,sizeof(n),"%s_PHY",tag);
        p445_mmio_range(stage,n,p445_phy,0x800U);
    }
    snprintf(n,sizeof(n),"%s_SEL",tag);
    p445_selectors(stage,&ctrl->hw,n);
}

static void p445_collect_regs(struct p445_supply_rec *r, u32 cap, u32 *n,
        struct dsi_regulator_info *info, u32 group)
{
    u32 i;
    int en,use,uv;
    if (!r || !n || !info) return;
    for (i=0; i<info->count && *n<cap; i++) {
        struct dsi_vreg *v=&info->vregs[i];
        struct p445_supply_rec *o;
        if (!v->vreg) continue;
        o=&r[*n];
        en=use=uv=-ENODATA;
        a52_p445_regulator_state(v->vreg,&en,&use,&uv);
        memset(o,0,sizeof(*o));
        strlcpy(o->name,v->vreg_name,sizeof(o->name));
        o->group=group; o->group_refcount=info->refcount;
        o->enabled=en; o->use_count=use; o->voltage=uv;
        o->min_uv=v->min_voltage; o->max_uv=v->max_voltage;
        (*n)++;
    }
}

static void p445_power(u32 stage, struct dsi_display *display, u32 tag,
        const char *secname)
{
    struct p445_power_meta m;
    struct p445_supply_rec *r;
    struct dsi_panel *panel;
    struct dsi_ctrl *ctrl=NULL;
    struct msm_dsi_phy *phy=NULL;
    u32 n=0;
    if (!display || !display->panel) return;
    panel=display->panel;
    if (display->ctrl_count) {
        ctrl=display->ctrl[0].ctrl;
        phy=display->ctrl[0].phy;
    }
    memset(&m,0,sizeof(m));
    m.tag=tag;
    m.gate=(u32)atomic_read(&p445_gate);
    m.rung=(u32)atomic_read(&p445_rung);
    m.panel_initialized=panel->panel_initialized;
    m.ulps_feature=panel->ulps_feature_enabled;
    m.ulps_suspend=panel->ulps_suspend_enabled;
    m.panel_supply_refcount=panel->power_info.refcount;
    if (ctrl) {
        m.ctrl_digital_refcount=ctrl->pwr_info.digital.refcount;
        m.ctrl_host_refcount=ctrl->pwr_info.host_pwr.refcount;
    }
    if (phy) {
        m.phy_digital_refcount=phy->pwr_info.digital.refcount;
        m.phy_pwr_refcount=phy->pwr_info.phy_pwr.refcount;
    }
#if defined(CONFIG_DISPLAY_SAMSUNG)
    if (panel->panel_private)
        m.samsung_state=((struct samsung_display_driver_data *)panel->panel_private)->panel_state;
#endif
    a52_p445_clk_client_snapshot(display->dsi_clk_handle,&m.core_ref,&m.link_ref,
        &m.core_state,&m.link_state);
    a52_p445_pll10_snapshot(0,&m.pll_handoff,&m.pll_on,&m.pll_prepare,
        &m.pll_enable,&m.pll_lock,&m.pll_resource_ref);
    a52_p445_refgen_snapshot(&m.refgen_registered,&m.refgen_enabled,
        &m.refgen_use_count,&m.refgen_pwrdwn);
    p445_hdr()->panel_initialized=m.panel_initialized;
    p445_hdr()->panel_ulps_feature=m.ulps_feature;
    p445_hdr()->panel_ulps_suspend=m.ulps_suspend;
    p445_hdr()->samsung_panel_state=m.samsung_state;
    p445_hdr()->core_ref=m.core_ref; p445_hdr()->link_ref=m.link_ref;
    p445_hdr()->core_clk_state=m.core_state; p445_hdr()->link_clk_state=m.link_state;
    p445_hdr()->pll_handoff=m.pll_handoff; p445_hdr()->pll_on=m.pll_on;
    p445_hdr()->pll_prepare=m.pll_prepare; p445_hdr()->pll_enable=m.pll_enable;
    p445_hdr()->pll_lock=m.pll_lock; p445_hdr()->pll_resource_ref=m.pll_resource_ref;
    p445_hdr()->refgen_registered=m.refgen_registered;
    p445_hdr()->refgen_enabled=m.refgen_enabled;
    p445_hdr()->refgen_use_count=m.refgen_use_count;
    p445_hdr()->refgen_pwrdwn=m.refgen_pwrdwn;
    a52_p445_store_section(stage,P445_TYPE_META,secname,tag,0,&m,sizeof(m));
    r=kcalloc(32U,sizeof(*r),GFP_KERNEL);
    if (!r) return;
    p445_collect_regs(r,32U,&n,&panel->power_info,P445_SUPPLY_PANEL);
    if (ctrl) {
        p445_collect_regs(r,32U,&n,&ctrl->pwr_info.digital,P445_SUPPLY_CTRL_DIGITAL);
        p445_collect_regs(r,32U,&n,&ctrl->pwr_info.host_pwr,P445_SUPPLY_CTRL_HOST);
    }
    if (phy) {
        p445_collect_regs(r,32U,&n,&phy->pwr_info.digital,P445_SUPPLY_PHY_DIGITAL);
        p445_collect_regs(r,32U,&n,&phy->pwr_info.phy_pwr,P445_SUPPLY_PHY_PWR);
    }
    if (n) {
        char sn[24];
        snprintf(sn,sizeof(sn),"SUPPLY_%u",tag);
        a52_p445_store_section(stage,P445_TYPE_META,sn,n,0,r,n*sizeof(r[0]));
    }
    kfree(r);
}

void a52_p445_runtime_census(struct dsi_display *display)
{
    struct p445_devrec *r;
    struct p445_bound_ctx c;
    char *d;
    int n;
    if (!display || atomic_read(&p445_state)!=0) return;
    r=kcalloc(192U,sizeof(*r),GFP_KERNEL);
    if (r) {
        c.r=r; c.n=0; c.max=192U;
        bus_for_each_dev(&platform_bus_type,NULL,&c,p445_bound_one);
        if (c.n) a52_p445_store_section(P445_STAGE_CENSUS,P445_TYPE_META,
            "BOUND_PLATFORM",c.n,0,r,c.n*sizeof(*r));
        kfree(r);
    }
    d=kcalloc(96U,64U,GFP_KERNEL);
    if (d) {
        n=a52_p445_deferred_snapshot(d,64U,96U);
        if (n>0) a52_p445_store_section(P445_STAGE_CENSUS,P445_TYPE_META,
            "DEVICES_DEFERRED",(u32)n,64U,d,(u32)n*64U);
        kfree(d);
    }
    p445_power(P445_STAGE_CENSUS,display,0U,"POWER_CENSUS");
    p445_checkpoint();
}
EXPORT_SYMBOL_GPL(a52_p445_runtime_census);

void a52_p445_sync_pre(struct dsi_display *display)
{
    if (!display || !display->ctrl_count || !display->ctrl[0].ctrl ||
        atomic_read(&p445_state)!=0) return;
    p445_ctrl=display->ctrl[0].ctrl;
    p445_hdr()->predeep_ns=ktime_get_ns();
    p445_compact(P445_STAGE_PRE,p445_ctrl,"PRE");
    p445_power(P445_STAGE_PRE,display,1U,"POWER_PRE");
    p445_checkpoint();
}
EXPORT_SYMBOL_GPL(a52_p445_sync_pre);

void a52_p445_display_snapshot(struct dsi_display *display, u32 tag)
{
    const char *name;
    if (!display || !display->ctrl_count || !display->ctrl[0].ctrl) return;
    name = tag==2U ? "R2_ENTRY" : (tag==3U ? "R2_LINKOFF" : "R2_LINKON");
    p445_compact(P445_STAGE_R2_PRE,display->ctrl[0].ctrl,name);
    p445_power(P445_STAGE_R2_PRE,display,tag,name);
    if (tag==3U) {
        struct p445_header *h=p445_hdr();
        h->r2_valid=1U;
        h->r2_flags=0U;
        if (h->pll_handoff) {
            h->r2_flags|=P445_R2F_HANDOFF_STUCK;
            pr_err("P445 R2 INVALID: handoff_resources still true after LINK OFF\n");
        }
        if (h->pll_prepare) h->r2_flags|=P445_R2F_VCO_PREP_STUCK;
        if (h->link_ref) h->r2_flags|=P445_R2F_LINK_REF_STUCK;
        if (h->r2_flags) h->r2_valid=0U;
    }
    p445_checkpoint();
}
EXPORT_SYMBOL_GPL(a52_p445_display_snapshot);

int a52_p445_gate_check(const struct mipi_dsi_msg *msg)
{
    int g=atomic_read(&p445_gate);
    const u8 *p;
    if (g==P445_GATE_OFF) return 0;
    if (g==P445_GATE_BLOCK) return -ECANCELED;
    if (!msg || msg->type!=0x29 || msg->tx_len!=3 || !msg->tx_buf)
        return -ECANCELED;
    p=msg->tx_buf;
    return (p[0]==0xf0 && p[1]==0x5a && p[2]==0x5a) ? 0 : -ECANCELED;
}
EXPORT_SYMBOL_GPL(a52_p445_gate_check);

bool a52_p445_samsung_blocked(void)
{
    return atomic_read(&p445_gate)==P445_GATE_BLOCK;
}
EXPORT_SYMBOL_GPL(a52_p445_samsung_blocked);

bool a52_p445_abort_pending(void)
{
    return atomic_read(&p445_gate)==P445_GATE_BLOCK;
}
EXPORT_SYMBOL_GPL(a52_p445_abort_pending);

u32 a52_p445_rung(void)
{
    return (u32)atomic_read(&p445_rung);
}
EXPORT_SYMBOL_GPL(a52_p445_rung);

void a52_p445_capture_kickoff_ctx(struct dsi_ctrl *ctrl,
        struct dsi_ctrl_cmd_dma_info *cmd_mem, u32 hw_flags)
{
    if (!a52_p445_active() || atomic_read(&p445_rung)!=0 || !ctrl || !cmd_mem)
        return;
    p445_saved_cmd_mem=*cmd_mem;
    p445_saved_hw_flags=hw_flags;
    p445_saved_cmd_valid=true;
}
EXPORT_SYMBOL_GPL(a52_p445_capture_kickoff_ctx);

bool a52_p445_should_r1(void)
{
#if P445_KIND == 1
    return a52_p445_active() && atomic_read(&p445_rung)==0 && p445_hdr()->r0_ret==0;
#else
    return false;
#endif
}
EXPORT_SYMBOL_GPL(a52_p445_should_r1);

void a52_p445_begin_r1(struct dsi_ctrl *ctrl)
{
    if (!ctrl) return;
    atomic_set(&p445_rung,1);
    p445_hdr()->rung=1;
    p445_compact(P445_STAGE_R1_PRE,ctrl,"R1_PRE");
    p445_checkpoint();
}
EXPORT_SYMBOL_GPL(a52_p445_begin_r1);

void a52_p445_after_r1_reset(struct dsi_ctrl *ctrl)
{
    if (!ctrl) return;
    p445_compact(P445_STAGE_R1_PRE,ctrl,"R1_RESET");
    p445_checkpoint();
}
EXPORT_SYMBOL_GPL(a52_p445_after_r1_reset);

int a52_p445_r1_rekick(struct dsi_ctrl *ctrl)
{
    if (!ctrl || !p445_saved_cmd_valid || !ctrl->hw.ops.kickoff_command)
        return -ENODATA;
    ctrl->hw.ops.kickoff_command(&ctrl->hw,&p445_saved_cmd_mem,p445_saved_hw_flags);
    return 0;
}
EXPORT_SYMBOL_GPL(a52_p445_r1_rekick);

bool a52_p445_need_r2(void)
{
#if P445_KIND == 1
    return a52_p445_active() && atomic_read(&p445_gate)==P445_GATE_BLOCK &&
        atomic_read(&p445_rung)==1 && p445_hdr()->r1_ret==0;
#else
    return false;
#endif
}
EXPORT_SYMBOL_GPL(a52_p445_need_r2);

void a52_p445_begin_r2(void)
{
    atomic_set(&p445_rung,2);
    p445_hdr()->rung=2;
    p445_sync_header();
}
EXPORT_SYMBOL_GPL(a52_p445_begin_r2);

void a52_p445_gate_diag(void)
{
    atomic_set(&p445_gate,P445_GATE_DIAG);
    p445_hdr()->gate=P445_GATE_DIAG;
    p445_sync_header();
}
EXPORT_SYMBOL_GPL(a52_p445_gate_diag);

void a52_p445_gate_block(void)
{
    atomic_set(&p445_gate,P445_GATE_BLOCK);
    p445_hdr()->gate=P445_GATE_BLOCK;
    p445_sync_header();
}
EXPORT_SYMBOL_GPL(a52_p445_gate_block);

void a52_p445_gate_off(void)
{
    atomic_set(&p445_gate,P445_GATE_OFF);
    p445_hdr()->gate=P445_GATE_OFF;
    p445_sync_header();
}
EXPORT_SYMBOL_GPL(a52_p445_gate_off);

int a52_p445_get_diag_msg(struct mipi_dsi_msg *msg, u8 payload[3])
{
    if (!msg || !payload || !p445_saved_msg.tx_len) return -ENODATA;
    *msg=p445_saved_msg;
    memcpy(payload,p445_saved_payload,3);
    msg->tx_buf=payload;
    msg->tx_len=3;
    msg->rx_buf=NULL;
    msg->rx_len=0;
    return 0;
}
EXPORT_SYMBOL_GPL(a52_p445_get_diag_msg);

bool a52_p445_r2_succeeded(void)
{
    return p445_hdr()->winner==P445_WIN_R2;
}
EXPORT_SYMBOL_GPL(a52_p445_r2_succeeded);

static void p445_print_hot(void)
{
#if P445_KIND == 2
    int i, n=atomic_read(&p445_hot_count);
    if (n>P445_MAX_HOT) n=P445_MAX_HOT;
    for(i=0;i<n;i++) {
        const struct p445_hot *s=&p445_hot[i];
        pr_info("P445 S i=%d p=%u dt=%llu st=%08x/%08x ck=%08x/%08x in=%08x/%08x ln=%08x/%08x db=%08x/%08x crc=%08x\n",
            i,s->point,(unsigned long long)s->dt_ns,
            s->status,s->status,s->clk_status,s->clk_status,
            s->int_ctrl,s->int_ctrl,s->lane_status,s->lane_status,
            s->dbg171,s->dbg171,s->crc32);
    }
#endif
}

void a52_p445_terminal(struct dsi_ctrl *ctrl, int ret)
{
    struct p445_header *h;
    u32 irq,rung,stage;
    const char *tag;
    if (!ctrl || !p445_image) return;
    h=p445_hdr();
    rung=(u32)atomic_read(&p445_rung);
    if ((rung==0 && h->r0_ret!=-ENODATA) ||
        (rung==1 && h->r1_ret!=-ENODATA) ||
        (rung==2 && h->r2_ret!=-ENODATA)) return;
    irq=(u32)atomic_read(&ctrl->dma_irq_trig);
    h->terminal_ns=ktime_get_ns(); h->terminal_ret=ret; h->terminal_irq=irq;
    if (rung==0) {
        h->r0_ret=ret; h->r0_irq=irq; h->r0_lane=DSI_R32(&ctrl->hw,DSI_LANE_STATUS);
        stage=P445_STAGE_R0_TERM; tag="R0_TERM";
    } else if (rung==1) {
        h->r1_ret=ret; h->r1_irq=irq; h->r1_lane=DSI_R32(&ctrl->hw,DSI_LANE_STATUS);
        stage=P445_STAGE_R1_TERM; tag="R1_TERM";
    } else {
        h->r2_ret=ret; h->r2_irq=irq; h->r2_lane=DSI_R32(&ctrl->hw,DSI_LANE_STATUS);
        stage=P445_STAGE_R2_TERM; tag="R2_TERM";
    }
    p445_print_hot();
    p445_compact(stage,ctrl,tag);
    if (ret > 0) {
        h->winner = rung==0 ? P445_WIN_R0 : (rung==1 ? P445_WIN_R1 : P445_WIN_R2);
        atomic_set(&p445_gate,P445_GATE_OFF); h->gate=P445_GATE_OFF;
        atomic_set(&p445_state,2);
    } else if (rung==0) {
#if P445_KIND == 2
        atomic_set(&p445_state,2);
#else
        /* GKI proceeds immediately to R1 in the held controller context. */
#endif
    } else if (rung==1) {
        atomic_set(&p445_gate,P445_GATE_BLOCK); h->gate=P445_GATE_BLOCK;
    } else {
        atomic_set(&p445_gate,P445_GATE_BLOCK); h->gate=P445_GATE_BLOCK;
        atomic_set(&p445_state,2);
    }
    p445_sync_header(); p445_checkpoint();
#if P445_KIND == 2
    pr_info("P445 TERM rung=%u ret=%d irq=%u win=%u gate=%u sec=%u used=%u crc=%08x\n",
        rung,ret,irq,h->winner,h->gate,h->section_count,h->used_bytes,h->hdr_crc32);
#endif
}
EXPORT_SYMBOL_GPL(a52_p445_terminal);

static void p445_recompile_scan_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(p445_recompile_scan_work,p445_recompile_scan_workfn);
static void p445_recompile_scan_workfn(struct work_struct *work)
{
    struct task_struct *g;
    bool found=false;
    (void)work;
    rcu_read_lock();
    for_each_process(g) {
        if (!strncmp(g->comm,"dex2oat",7) || !strncmp(g->comm,"odrefresh",9)) {
            found=true; break;
        }
    }
    rcu_read_unlock();
    if (found && !(p445_hdr()->flags & P445_F_RECOMPILE)) {
        p445_hdr()->flags |= P445_F_RECOMPILE;
        p445_sync_header();
    }
    if (atomic_read(&p445_state)==0)
        mod_delayed_work(system_unbound_wq,&p445_recompile_scan_work,msecs_to_jiffies(100));
}

static void p445_sd_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(p445_sd_work,p445_sd_workfn);
static void p445_sd_workfn(struct work_struct *work)
{
    struct p445_sdrec *r;
    unsigned int n=0, minor;
    int partno;
    struct gendisk *gd;
    (void)work;
    r=kcalloc(64U,sizeof(*r),GFP_KERNEL);
    if (!r) return;
    for (minor=0; minor<256 && n<64U; minor++) {
        gd=get_gendisk(MKDEV(MMC_BLOCK_MAJOR,minor),&partno);
        if (!gd) continue;
        r[n].minor=minor; r[n].partno=partno; r[n].sectors=get_capacity(gd);
        strlcpy(r[n].name,gd->disk_name,sizeof(r[n].name)); n++;
        put_disk(gd);
    }
    if (n) a52_p445_store_section(P445_STAGE_SD,P445_TYPE_SD,"MMC_MAJOR179",n,0,r,n*sizeof(r[0]));
    else { u32 zero=0; a52_p445_store_section(P445_STAGE_SD,P445_TYPE_SD,"MMC_MAJOR179_ZERO",0,0,&zero,sizeof(zero)); }
    kfree(r);
#if P445_KIND == 1
    p445_checkpoint();
#endif
}

static ssize_t p445_raw_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    if (!p445_image) return -ENODATA;
    return simple_read_from_buffer(buf,count,ppos,p445_image,P445_IMAGE_BYTES);
}
static int p445_seq_show(struct seq_file *m, void *v)
{
    struct p445_header *h=p445_hdr(); unsigned int i,n;
    seq_printf(m,"P445 kind=%u boot=%llx flags=%x state=%u gate=%u rung=%u win=%u r2valid=%u r2flags=%x sec=%u used=%u r0=%d/%u r1=%d/%u r2=%d/%u hdr_crc=%08x\n",
        h->kind,(unsigned long long)h->boot_id,h->flags,h->state,h->gate,h->rung,h->winner,h->r2_valid,h->r2_flags,h->section_count,h->used_bytes,h->r0_ret,h->r0_irq,h->r1_ret,h->r1_irq,h->r2_ret,h->r2_irq,h->hdr_crc32);
    n=min_t(unsigned int,h->section_count,P445_MAX_SECTIONS);
    for(i=0;i<n;i++) seq_printf(m,"SEC i=%u st=%u ty=%u name=%s off=%x len=%x crc=%08x z=%u ff=%u a0=%x a1=%x commit=%08x\n",
        i,h->sec[i].stage,h->sec[i].type,h->sec[i].name,h->sec[i].offset,h->sec[i].length,h->sec[i].crc32,h->sec[i].zero_count,h->sec[i].ffff_count,h->sec[i].aux0,h->sec[i].aux1,h->sec[i].commit);
    return 0;
}
static int p445_seq_open(struct inode *inode, struct file *file) { return single_open(file,p445_seq_show,NULL); }
static const struct proc_ops p445_raw_fops = {
    .proc_read=p445_raw_read,.proc_lseek=default_llseek,
};
static const struct proc_ops p445_seq_fops = {
    .proc_open=p445_seq_open,.proc_read=seq_read,.proc_lseek=seq_lseek,.proc_release=single_release,
};


/* A52_CRASH1_FRAME_EVENT_PERSIST_V1:
 * Fail-stop diagnostics only. No raw F0, no panel register writes.
 * All data stored with P445 section CRC and 16 redundant copies.
 */
#include <linux/kmsg_dump.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <asm/ptrace.h>

#define CRASH1_REPLICAS 16U
#define CRASH1_MAX_EVENTS 24
#define CRASH1_KMSG_BYTES 4096U
struct crash1_fevent {
    u32 magic, version, seq, tag, event, crtc_id, pid, reserved;
    u64 ns, cbdata, crtc, fevent;
    u64 ns_inv;
};
struct crash1_fault {
    u32 magic, version, pid, esr;
    u64 far, pc, lr, sp, pstate, ns;
    u64 far_inv, pc_inv, lr_inv;
    char comm[TASK_COMM_LEN];
};
static atomic_t crash1_event_count = ATOMIC_INIT(0);
static atomic_t crash1_fault_once = ATOMIC_INIT(0);
static atomic_t crash1_dump_once = ATOMIC_INIT(0);
static struct crash1_fault crash1_fault_copies[CRASH1_REPLICAS];
static char crash1_kmsg[CRASH1_KMSG_BYTES];
static char crash1_kmsg_copies[CRASH1_REPLICAS][CRASH1_KMSG_BYTES];
static struct kmsg_dumper crash1_dumper;
static int crash1_dumper_registered;

void a52_crash1_event(u32 tag, u32 event, const void *cbdata,
                      const void *crtc, const void *fevent, u32 crtc_id)
{
    struct crash1_fevent r = {0};
    struct crash1_fevent copies[CRASH1_REPLICAS];
    int seq, i;

    /* Remain passive; record the first several event transitions only. */
    seq = atomic_inc_return(&crash1_event_count);
    if (seq > CRASH1_MAX_EVENTS || !p445_image) return;
    r.magic = 0x31564546U;  /* FEV1 */
    r.version = 1U;
    r.seq = seq;
    r.tag = tag;
    r.event = event;
    r.crtc_id = crtc_id;
    r.pid = task_pid_nr(current);
    r.ns = ktime_get_ns();
    r.cbdata = (u64)(unsigned long)cbdata;
    r.crtc = (u64)(unsigned long)crtc;
    r.fevent = (u64)(unsigned long)fevent;
    r.ns_inv = ~r.ns;
    for (i = 0; i < CRASH1_REPLICAS; i++) copies[i] = r;
    (void)a52_p445_store_section(0xc1a50001U, 0xc1a5U,
                  "CR1_FRAME_EVENT", (u32)seq, tag, copies, sizeof(copies));
}
EXPORT_SYMBOL_GPL(a52_crash1_event);

/* Called before the ordinary do_mem_abort() diagnostic logging. */
void a52_crash1_fault(unsigned long far, unsigned int esr,
                      const struct pt_regs *regs)
{
    struct crash1_fault r = {0};
    int i;

    if (!regs || user_mode(regs) || !p445_image ||
        atomic_cmpxchg(&crash1_fault_once, 0, 1) != 0) return;
    r.magic = 0x31544c46U; /* FLT1 */
    r.version = 1U;
    r.pid = task_pid_nr(current);
    r.esr = esr;
    r.far = far;
    r.pc = regs->pc;
    r.lr = regs->regs[30];
    r.sp = regs->sp;
    r.pstate = regs->pstate;
    r.ns = ktime_get_ns();
    r.far_inv = ~r.far;
    r.pc_inv = ~r.pc;
    r.lr_inv = ~r.lr;
    memcpy(r.comm, current->comm, TASK_COMM_LEN);
    for (i = 0; i < CRASH1_REPLICAS; i++) crash1_fault_copies[i] = r;
    (void)a52_p445_store_section(0xc1a50002U, 0xc1a5U,
                  "CR1_ARM64_FAULT", r.esr, r.pid,
                  crash1_fault_copies, sizeof(crash1_fault_copies));
    pr_emerg("CRASH1 FAULT PID=%u FAR=%016llx PC=%016llx LR=%016llx ESR=%08x\n",
             r.pid, r.far, r.pc, r.lr, r.esr);
}
EXPORT_SYMBOL_GPL(a52_crash1_fault);

static void crash1_dump(struct kmsg_dumper *dumper,
                        enum kmsg_dump_reason reason)
{
    size_t len = 0;
    int i;

    if (reason != KMSG_DUMP_OOPS && reason != KMSG_DUMP_PANIC)
        return;
    if (!p445_image ||
        atomic_cmpxchg(&crash1_dump_once, 0, 1) != 0)
        return;
    memset(crash1_kmsg, 0, sizeof(crash1_kmsg));
    kmsg_dump_rewind(dumper);
    if (!kmsg_dump_get_buffer(dumper, false, crash1_kmsg,
                              sizeof(crash1_kmsg), &len) || !len)
        return;
    for (i = 0; i < CRASH1_REPLICAS; i++)
        memcpy(crash1_kmsg_copies[i], crash1_kmsg, sizeof(crash1_kmsg));
    (void)a52_p445_store_section(0xc1a50003U, 0xc1a5U,
                  "CR1_OOPS_KMSG", (u32)reason, (u32)len,
                  crash1_kmsg_copies, sizeof(crash1_kmsg_copies));
}


/* A52_FENCE1E_F0_TRACE_16X_V1. Passive, bounded 16x diagnostics. */
#include <linux/atomic.h>
#include <linux/ktime.h>
struct a52_fence1e_f0_entry {
    u32 magic, version, sequence, attempt, marker, arg0, arg1, marker_inv;
    u64 ns, ns_inv;
};

/* A52_FENCE1F_POSTCOLD_F0_REBLOCK_V2
 * 0 untouched, 1 first cold-ON test active, 2 F0 failed/block restored,
 * 3 first cold-ON F0 succeeded; do not interfere with later traffic.
 */
static atomic_t a52_fence1f_window = ATOMIC_INIT(0);
void a52_fence1f_cold_reopen(void)
{
    if (!a52_p445_samsung_blocked() ||
        atomic_cmpxchg(&a52_fence1f_window, 0, 1) != 0)
        return;
    a52_p445_gate_off();
    pr_warn("FENCE1F postcold gate OPEN: first successful P6/prepare; real Samsung ON allowed\n");
}
EXPORT_SYMBOL_GPL(a52_fence1f_cold_reopen);

static atomic_t a52_fence1e_f0_seq = ATOMIC_INIT(0);
static atomic_t a52_fence1e_f0_attempt = ATOMIC_INIT(0);
static void a52_fence1e_record_f0(u32 marker, u32 arg0, u32 arg1)
{
    struct a52_fence1e_f0_entry e = {0};
    struct a52_fence1e_f0_entry copies[16];
    int seq, attempt, i;

    /* 0x1a0 = F0 detected; 0x1a9 = DMA wait result when active;
     * 0x1ac = original dsi_message_tx error/return label.
     * Legacy Phase446 callbacks for other events remain disabled.
     */
    if (marker != 0x1a0U && marker != 0x1a9U &&
        marker != 0x1acU)
        return;
    if (!p445_image)
        return;
    seq = atomic_inc_return(&a52_fence1e_f0_seq);
    if (seq > 96)
        return;
    if (marker == 0x1a0U)
        attempt = atomic_inc_return(&a52_fence1e_f0_attempt);
    else
        attempt = atomic_read(&a52_fence1e_f0_attempt);
    e.magic = 0x45314630U;  /* ASCII 0F1E, decoder marker */
    e.version = 1U;
    e.sequence = (u32)seq;
    e.attempt = (u32)attempt;
    e.marker = marker;
    e.arg0 = arg0;
    e.arg1 = arg1;
    e.marker_inv = ~marker;
    e.ns = ktime_get_ns();
    e.ns_inv = ~e.ns;
    for (i = 0; i < 16; i++)
        copies[i] = e;
    (void)a52_p445_store_section(0xe10f0001U, 0xe10fU,
                   "F1E_F0_TX", (u32)seq, marker,
                   copies, sizeof(copies));
    /* First postcold real F0 return is the decisive result.
     * Preserve Phase445's original fail-stop if the cold attempt also stalls.
     */
    if (marker == 0x1acU && (s32)arg0 < 0 &&
        atomic_cmpxchg(&a52_fence1f_window, 1, 2) == 1) {
        a52_p445_gate_block();
        pr_warn("FENCE1F postcold F0 failed rc=%d: Phase445 gate BLOCK restored\n",
                (s32)arg0);
    } else if (marker == 0x1acU && (s32)arg0 >= 0 &&
               atomic_cmpxchg(&a52_fence1f_window, 1, 3) == 1) {
        pr_warn("FENCE1F postcold F0 completed rc=%d: gate left OPEN\n",
                (s32)arg0);
    }
    pr_warn("FENCE1E F0 attempt=%d seq=%d stage=%03x arg0=%08x arg1=%08x\n",
            attempt, seq, marker, arg0, arg1);
}

static int __init p445_init(void)
{
    struct p445_header *h;
    BUILD_BUG_ON(sizeof(struct p445_header)>P445_HEADER_BYTES);
    p445_image=vzalloc(P445_IMAGE_BYTES); if(!p445_image) return 0;
    h=p445_hdr(); h->magic=P445_MAGIC; h->version=P445_VERSION; h->kind=P445_KIND;
#if P445_KIND == 1
    h->flags |= P445_F_DISK_DISABLED_OVERLAP;
#endif
    h->boot_id=get_random_u64(); if(!h->boot_id) h->boot_id=ktime_get_ns()^P445_MAGIC;
    h->init_ns=ktime_get_ns(); h->terminal_ret=-ENODATA; h->r0_ret=-ENODATA; h->r1_ret=-ENODATA; h->r2_ret=-ENODATA; h->r2_valid=1U; h->disk_submit_rc=-ENODATA; h->disk_flush_rc=-ENODATA;
    atomic_set(&p445_used,P445_HEADER_BYTES); atomic_set(&p445_sections,0);
#if P445_KIND == 1
    p445_ram=ioremap_cache(P445_RAM_PHYS,P445_RAM_BYTES);
    if(p445_ram) h->flags|=P445_F_RAM_OK;
#endif
    p445_sync_header();
    crash1_dumper.dump = crash1_dump;
    crash1_dumper_registered = !kmsg_dump_register(&crash1_dumper);
    pr_info("A52CRASH1 dumper_registered=%d P445_RAM=%d\n",
            crash1_dumper_registered, !!p445_ram);
    mod_delayed_work(system_unbound_wq,&p445_recompile_scan_work,msecs_to_jiffies(100));
    mod_delayed_work(system_unbound_wq,&p445_sd_work,msecs_to_jiffies(15000));
    pr_info("A52_PHASE445_RECOVERY_LADDER_V1 kind=%u boot=%llx ram=%d\n",P445_KIND,(unsigned long long)h->boot_id,!!p445_ram);
    return 0;
}
subsys_initcall(p445_init);
static int __init p445_proc_init(void)
{
    proc_create("a52_phase445_raw",0444,NULL,&p445_raw_fops);
    proc_create("a52_phase445",0444,NULL,&p445_seq_fops);
    return 0;
}
late_initcall(p445_proc_init);

static const char p445_marker[] __used = "A52_PHASE445_RECOVERY_LADDER_V1";


/* A52_PHASE446_EARLY_SPLASH_HEARTBEAT_V1
 * Early continuous-splash heartbeat and takeover microscope.
 * Diagnostic MMIO is gated by always-accessible GCC/DISPCC status before
 * touching MDP/INTF/PP/DSI. Fixed records are mirrored into reserved RAM and
 * compact P446 copies are forwarded to the Phase414 Samsung-debug transport.
 */
#define P446_MAGIC 0x363434504d4f4346ULL /* FCOMP446 */
#define P446_VERSION 1U
#define P446_RAM_PHYS 0xB1800000ULL
#define P446_RAM_BYTES SZ_1M
#define P446_HDR_BYTES SZ_4K
#define P446_MAX_REC 1536U
#define P446_REC_COMMIT 0x446c0de5U
#define P446_HDR_COMMIT 0x446b0071U
#define P446_MDP_PHYS 0x0ae00000ULL
#define P446_MDP_BYTES 0x00085000U
#define P446_VBIF_PHYS 0x0aeb0000ULL
#define P446_VBIF_BYTES 0x00003000U
#define P446_DISPCC_PHYS 0x0af00000ULL
#define P446_DISPCC_BYTES 0x00003000U
#define P446_GCC_PHYS 0x00100000ULL
#define P446_GCC_BYTES 0x00018000U
#define P446_REFGEN_PHYS 0x088e7000ULL
#define P446_REFGEN_BYTES 0x100U
#define P446_PHY_PHYS 0x0ae94000ULL
#define P446_PHY_BYTES 0x1000U
#define P446_F_PERIODIC BIT(0)
#define P446_F_SAFE_MDP BIT(1)
#define P446_F_SAFE_DSI BIT(2)
#define P446_F_MDP_READ BIT(3)
#define P446_F_DSI_READ BIT(4)
#define P446_F_SMMU_VALID BIT(5)
#define P446_F_IOVA_VALID BIT(6)
#define P446_F_CB2_REUSE BIT(7)
#define P446_EVT_PERIODIC 0x100U

struct p446_header {
    u64 magic;
    u32 version, record_bytes, capacity, count, dropped;
    u64 boot_id, init_ns, first_ns, last_ns;
    u32 cb2_param, actual_cb, flags, hdr_crc32, commit;
    u8 reserved[48];
} __packed;

struct p446_smmu_cache {
    u32 valid, smr, s2cr, cb, sctlr, tcr, fsr, aux;
    u64 ttbr0, iova, phys;
    u32 iova_valid, actual_cb;
};

struct p446_bus_snap {
    u32 valid, handle;
    s32 curr;
    u32 num_paths;
    u64 ab, ib;
} __packed;

struct p446_rec {
    u64 magic, ns;
    u32 seq, event, flags, aux0, aux1;
    u32 frame, tear, top08, top0c, top10, top14;
    u32 intf[7];
    u32 clk[8];
    u32 refgen, pll_lock;
    u32 vbif[4];
    u32 dsi[4];
    u32 pp[4];
    struct p446_bus_snap bus[4];
    u32 sspp[4][6];
    u32 ctl[7];
    u32 lm[3];
    u32 smr, s2cr, cb, sctlr, tcr, fsr, smmu_aux;
    u64 ttbr0, iova, phys;
    u32 cb2_param, actual_cb;
    /* Phase446d: CMD/CFG/M/N/D for MDP, BYTE0, PCLK0, ESC0, VSYNC, AHB. */
    u32 rcg[30];
    /* PLL0: MODE,L,ALPHA,USER,CONFIG,CONFIG_U,TEST,TEST_U,STATUS. */
    u32 pll0[9];
    /* Phase446g: live TLMM gpio23 TE pad. */
    u32 te23_cfg, te23_intr_cfg;
    u32 crc32, commit;
} __packed;

static u8 *p446_image;
static void __iomem *p446_ram, *p446_mdp, *p446_vbif, *p446_dispcc, *p446_gcc;
static void __iomem *p446_refgen, *p446_phy, *p446_dsi, *p446_tlmm23;
static DEFINE_SPINLOCK(p446_lock);
static DEFINE_SPINLOCK(p446_meta_lock);
static struct p446_smmu_cache p446_smmu;
static atomic_t p446_seq = ATOMIC_INIT(0);
static atomic_t p446_drop = ATOMIC_INIT(0);
static atomic_t p446_burst = ATOMIC_INIT(0);
/* Phase446h: after panel handoff, never let recorder runtime-PM
 * sampling block record commit. Cached SMMU state remains available.
 */
static atomic_t p446h_late_safe = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(p446_burst_wq);
static struct task_struct *p446_burst_task;
static const char p446d_extra_marker[] __used = "A52_PHASE446D_BURST_PLL_PROVIDER_V2_CENTRAL";

static atomic_t p446b_first_safe = ATOMIC_INIT(0);
static u64 p446_start_ns;
static u32 p446_cb2_param = 1U, p446_actual_cb = ~0U;
extern void a52_ackfr_record(const char *fmt, ...);
extern void a52_p427_bus_get(u32 slot, u32 *valid, u32 *handle, s32 *curr,
                             u32 *num_paths, u64 *ab, u64 *ib);

static u32 p446_crc(const void *p, size_t n) { return crc32_le(~0U,p,n)^~0U; }
static inline u32 p446_r(void __iomem *b, u32 o) { return b ? readl_relaxed((u8 __iomem *)b+o) : ~0U; }
static void p446_flush(void __iomem *p,size_t n) { if(!p||!n)return; wmb(); __flush_dcache_area((void __force *)p,n); dsb(sy); }
void a52_p446_mark(u32 event,u32 aux0,u32 aux1);
static struct p446_header *p446_hdr(void) { return (struct p446_header *)p446_image; }

static void p446_sync_header(void)
{
    struct p446_header *h;
    if(!p446_image) return;
    h=p446_hdr(); h->count=min_t(u32,(u32)atomic_read(&p446_seq),P446_MAX_REC);
    h->dropped=(u32)atomic_read(&p446_drop); h->cb2_param=READ_ONCE(p446_cb2_param);
    h->actual_cb=READ_ONCE(p446_actual_cb); h->hdr_crc32=0; h->commit=P446_HDR_COMMIT;
    h->hdr_crc32=p446_crc(h,offsetof(struct p446_header,hdr_crc32));
    if(p446_ram){ memcpy_toio(p446_ram,p446_image,P446_HDR_BYTES); p446_flush(p446_ram,P446_HDR_BYTES); }
}

void a52_p446_set_cb2_state(u32 enabled, u32 actual_cb)
{
    WRITE_ONCE(p446_cb2_param,!!enabled); WRITE_ONCE(p446_actual_cb,actual_cb);
    p446_sync_header();
}
EXPORT_SYMBOL_GPL(a52_p446_set_cb2_state);

void a52_p446_register_dsi(void __iomem *base)
{
    WRITE_ONCE(p446_dsi,base);
    a52_ackfr_record("P446 DSI base=%px",base);
}
EXPORT_SYMBOL_GPL(a52_p446_register_dsi);

void a52_p446_set_late_safe(u32 enable)
{
    atomic_set(&p446h_late_safe, !!enable);
}
EXPORT_SYMBOL_GPL(a52_p446_set_late_safe);

void a52_p446_note_smmu(u32 event,u32 smr,u32 s2cr,u32 cb,u32 sctlr,u64 ttbr0,u32 tcr,u32 fsr,u32 aux)
{
    unsigned long f;
    spin_lock_irqsave(&p446_meta_lock,f);
    p446_smmu.valid=1; p446_smmu.smr=smr; p446_smmu.s2cr=s2cr; p446_smmu.cb=cb;
    p446_smmu.sctlr=sctlr; p446_smmu.ttbr0=ttbr0; p446_smmu.tcr=tcr; p446_smmu.fsr=fsr; p446_smmu.aux=aux;
    p446_smmu.actual_cb=cb;
    spin_unlock_irqrestore(&p446_meta_lock,f);
    WRITE_ONCE(p446_actual_cb,cb);
    a52_p446_mark(event,aux,0);
}
EXPORT_SYMBOL_GPL(a52_p446_note_smmu);

void a52_p446_note_iova(u32 event,u64 iova,u64 phys,u32 aux)
{
    unsigned long f;
    spin_lock_irqsave(&p446_meta_lock,f);
    p446_smmu.iova=iova; p446_smmu.phys=phys; p446_smmu.iova_valid=1;
    spin_unlock_irqrestore(&p446_meta_lock,f);
    a52_p446_mark(event,aux,0);
}
EXPORT_SYMBOL_GPL(a52_p446_note_iova);

extern int a52_p446_read_cb2(u32 *smr,u32 *s2cr,u32 *cb,u32 *sctlr,u64 *ttbr0,u32 *tcr,u32 *fsr);
/* A52_PHASE446B_LIVE_CB_READ */
static void a52_fence1e_record_f0(u32, u32, u32);
void a52_p446_mark(u32 event,u32 aux0,u32 aux1)
{
	/* A52_U2_AB_OBSERVER_QUIET_V1 */
	static bool a52_u2_ab_disable_a52_p446_mark = true;
	/* FENCE1E is independent of the quieted old sampler. */
	a52_fence1e_record_f0(event, aux0, aux1);
	if (!READ_ONCE(a52_u2_ab_disable_a52_p446_mark)) {

    static const u32 sspp_off[4]={0x5000,0x25000,0x27000,0x29000};
    struct p446_rec r; struct p446_smmu_cache sc; unsigned long f;
    u32 idx,gcc_ahb,disp_ahb,mdp_clk,gdsc,byte0,pclk0; bool safe_mdp,safe_dsi;
    void __iomem *dsi;
    if(!p446_image) return;
    memset(&r,0,sizeof(r)); r.magic=P446_MAGIC; r.ns=ktime_get_boottime_ns(); r.event=event; r.aux0=aux0; r.aux1=aux1;
    if(event==P446_EVT_PERIODIC) r.flags|=P446_F_PERIODIC;
    /* Phase446b: gcc_disp_ahb_clk is critical + HW-gated. Its CLK_OFF bit is
     * not a valid safety gate in HWCG mode. Read DISPCC unconditionally.
     */
    gcc_ahb=p446_r(p446_gcc,0x1700c);
    disp_ahb=p446_r(p446_dispcc,0x104c);
    mdp_clk=p446_r(p446_dispcc,0x1010);
    gdsc=p446_r(p446_dispcc,0x1004);
    byte0=p446_r(p446_dispcc,0x102c);
    pclk0=p446_r(p446_dispcc,0x100c);
    r.clk[0]=gcc_ahb; r.clk[1]=p446_r(p446_gcc,0x1701c);
    r.clk[2]=disp_ahb; r.clk[3]=mdp_clk; r.clk[4]=byte0; r.clk[5]=pclk0; r.clk[6]=gdsc;
    r.clk[7]=p446_r(p446_dispcc,0x1034);
    r.refgen=p446_r(p446_refgen,0x80); r.pll_lock=~0U;
    safe_mdp=!!(gdsc & BIT(31)) && !(disp_ahb & BIT(31)) && !(mdp_clk & BIT(31));
    safe_dsi=safe_mdp && !(byte0 & BIT(31)) && !(pclk0 & BIT(31));
    /* H00: first periodic sample that passes the corrected HWCG-safe gate. */
    if(safe_mdp && event==P446_EVT_PERIODIC &&
       atomic_cmpxchg(&p446b_first_safe,0,1)==0){
        r.event=0x120U; r.aux0=P446_EVT_PERIODIC; r.aux1=0U;
    }
    if(safe_mdp){
        u32 i,o; r.flags|=P446_F_SAFE_MDP|P446_F_MDP_READ;
        r.top08=p446_r(p446_mdp,0x1008); r.top0c=p446_r(p446_mdp,0x100c); r.top10=p446_r(p446_mdp,0x1010); r.top14=p446_r(p446_mdp,0x1014);
        r.intf[0]=p446_r(p446_mdp,0x6b800+0x000); r.intf[1]=p446_r(p446_mdp,0x6b800+0x0a8);
        r.frame=p446_r(p446_mdp,0x6b800+0x0ac); r.intf[2]=p446_r(p446_mdp,0x6b800+0x0b0);
        r.intf[3]=p446_r(p446_mdp,0x6b800+0x26c); r.tear=p446_r(p446_mdp,0x6b800+0x298); r.intf[4]=p446_r(p446_mdp,0x6b800+0x2b4);
        r.intf[5]=p446_r(p446_mdp,0x6b800+0x29c); r.intf[6]=p446_r(p446_mdp,0x6b800+0x2b8);
        /* Phase446b first-commit microscope. Binary layout stays 508 bytes.
         * pp[]: AUTOREFRESH, LINE_COUNT, INT_COUNT, OUT_LINE_COUNT.
         * sspp[][0..5]: SRC_SIZE, SRC2_ADDR, SRC0_ADDR, SRC_FORMAT,
         *                 SRC_YSTRIDE0, SRC_OP_MODE.
         * ctl[]: LAYER, EXT, EXT2, EXT3, FLUSH, START, STATUS.
         */
        r.pp[0]=p446_r(p446_mdp,0x71000+0x030); r.pp[1]=p446_r(p446_mdp,0x71000+0x02c); r.pp[2]=p446_r(p446_mdp,0x71000+0x014); r.pp[3]=p446_r(p446_mdp,0x71000+0x028);
        for(i=0;i<4;i++){ o=sspp_off[i]; r.sspp[i][0]=p446_r(p446_mdp,o+0x00); r.sspp[i][1]=p446_r(p446_mdp,o+0x1c); r.sspp[i][2]=p446_r(p446_mdp,o+0x14); r.sspp[i][3]=p446_r(p446_mdp,o+0x30); r.sspp[i][4]=p446_r(p446_mdp,o+0x24); r.sspp[i][5]=p446_r(p446_mdp,o+0x38); }
        r.ctl[0]=p446_r(p446_mdp,0x2000+0x000); r.ctl[1]=p446_r(p446_mdp,0x2000+0x040); r.ctl[2]=p446_r(p446_mdp,0x2000+0x070); r.ctl[3]=p446_r(p446_mdp,0x2000+0x0a0); r.ctl[4]=p446_r(p446_mdp,0x2000+0x018); r.ctl[5]=p446_r(p446_mdp,0x2000+0x01c); r.ctl[6]=p446_r(p446_mdp,0x2000+0x064);
        r.lm[0]=p446_r(p446_mdp,0x45000+0x000); r.lm[1]=p446_r(p446_mdp,0x45000+0x020); r.lm[2]=p446_r(p446_mdp,0x45000+0x004);
        r.vbif[0]=p446_r(p446_vbif,0x0000); r.vbif[1]=p446_r(p446_vbif,0x0190); r.vbif[2]=p446_r(p446_vbif,0x0200); r.vbif[3]=p446_r(p446_vbif,0x0550);
    }
    dsi=READ_ONCE(p446_dsi); if(safe_dsi){ r.pll_lock=p446_r(p446_phy,0xba0); } if(safe_dsi && dsi){ r.flags|=P446_F_SAFE_DSI|P446_F_DSI_READ; r.dsi[0]=p446_r(dsi,DSI_STATUS); r.dsi[1]=p446_r(dsi,DSI_LANE_STATUS); r.dsi[2]=p446_r(dsi,DSI_CLK_STATUS); r.dsi[3]=p446_r(dsi,DSI_INT_CTRL); }
    spin_lock_irqsave(&p446_meta_lock,f); sc=p446_smmu; spin_unlock_irqrestore(&p446_meta_lock,f);
    {
        u32 l_smr=~0U,l_s2cr=~0U,l_cb=~0U,l_sctlr=~0U,l_tcr=~0U,l_fsr=~0U;
        u64 l_ttbr0=~0ULL;
        int lrc=-1;
        bool smmu_internal=(event==1U || event==8U || event==9U ||
                            event==0x70U || event==0x71U ||
                            event==0x74U || event==0x75U);
        if(!smmu_internal && !atomic_read(&p446h_late_safe))
            lrc=a52_p446_read_cb2(&l_smr,&l_s2cr,&l_cb,&l_sctlr,&l_ttbr0,&l_tcr,&l_fsr);
        if(!lrc){
            r.flags|=P446_F_SMMU_VALID; r.smr=l_smr; r.s2cr=l_s2cr; r.cb=l_cb;
            r.sctlr=l_sctlr; r.ttbr0=l_ttbr0; r.tcr=l_tcr; r.fsr=l_fsr;
            r.smmu_aux=0x446b0001U;
        } else if(sc.valid){
            r.flags|=P446_F_SMMU_VALID; r.smr=sc.smr; r.s2cr=sc.s2cr; r.cb=sc.cb;
            r.sctlr=sc.sctlr; r.ttbr0=sc.ttbr0; r.tcr=sc.tcr; r.fsr=sc.fsr; r.smmu_aux=sc.aux;
        }
    }
    if(sc.iova_valid){ r.flags|=P446_F_IOVA_VALID; r.iova=sc.iova; r.phys=sc.phys; }
    { u32 bi; for (bi=0;bi<4;bi++) a52_p427_bus_get(bi,&r.bus[bi].valid,&r.bus[bi].handle,&r.bus[bi].curr,&r.bus[bi].num_paths,&r.bus[bi].ab,&r.bus[bi].ib); }
    r.cb2_param=READ_ONCE(p446_cb2_param); r.actual_cb=READ_ONCE(p446_actual_cb); if(r.cb2_param) r.flags|=P446_F_CB2_REUSE;
    idx=(u32)atomic_inc_return(&p446_seq)-1U; r.seq=idx; if(idx>=P446_MAX_REC){ atomic_inc(&p446_drop); p446_sync_header(); return; }
    /* A52_PHASE446D_POWER_CLOCK_MICROSCOPE_V1: matched per-record DISPCC RCG/PLL snapshot. */
    {
        static const u32 p446d_rcg_base[6] = {0x107cU,0x10c4U,0x1064U,0x10e0U,0x10acU,0x115cU};
        static const u32 p446d_pll_off[9] = {0x00U,0x04U,0x08U,0x0cU,0x10U,0x14U,0x18U,0x1cU,0x2cU};
        u32 gi, gj;
        for (gi = 0; gi < ARRAY_SIZE(p446d_rcg_base); gi++)
            for (gj = 0; gj < 5; gj++)
                r.rcg[gi * 5 + gj] = p446_r(p446_dispcc, p446d_rcg_base[gi] + gj * 4U);
        for (gi = 0; gi < ARRAY_SIZE(p446d_pll_off); gi++)
            r.pll0[gi] = p446_r(p446_dispcc, p446d_pll_off[gi]);
    }
    /* GPIO23: CFG @ 0x0F117000, INTR_CFG @ +0x8. FUNC_SEL is bits[5:2].
     * Lagoon gpio23 funcs[0]=GPIO, funcs[1]=MDP_VSYNC.
     */
    r.te23_cfg=p446_r(p446_tlmm23,0x0U);
    r.te23_intr_cfg=p446_r(p446_tlmm23,0x8U);
    r.commit=P446_REC_COMMIT; r.crc32=0; r.crc32=p446_crc(&r,sizeof(r));
    spin_lock_irqsave(&p446_lock,f);
    memcpy(p446_image+P446_HDR_BYTES+idx*sizeof(r),&r,sizeof(r));
    if(p446_ram){ void __iomem *dst=(u8 __iomem *)p446_ram+P446_HDR_BYTES+idx*sizeof(r); memcpy_toio(dst,&r,sizeof(r)); p446_flush(dst,sizeof(r)); }
    if(!p446_hdr()->first_ns) p446_hdr()->first_ns=r.ns; p446_hdr()->last_ns=r.ns;
    spin_unlock_irqrestore(&p446_lock,f); p446_sync_header();
    /* A52_PHASE446B_DEBUG_MIRROR_DROPPED: the Phase414 partition transport did not persist P446. */

	}
	return;
}
EXPORT_SYMBOL_GPL(a52_p446_mark);

static void p446_workfn(struct work_struct *w);
static DECLARE_DELAYED_WORK(p446_work,p446_workfn);
static void p446_workfn(struct work_struct *w)
{
	/* A52_U2_AB_OBSERVER_QUIET_V1 */
	static bool a52_u2_ab_disable_p446_workfn = true;
	if (!READ_ONCE(a52_u2_ab_disable_p446_workfn)) {

    u64 age; unsigned long d; (void)w; if (!atomic_read(&p446_burst)) a52_p446_mark(P446_EVT_PERIODIC,0,0); age=ktime_get_boottime_ns()-p446_start_ns;
    d=(age < 10000000000ULL)?msecs_to_jiffies(10):msecs_to_jiffies(1000); mod_delayed_work(system_unbound_wq,&p446_work,d);

	}
	return;
}

static int __init p446_early_init(void)
{
    struct p446_header *h;
    BUILD_BUG_ON(P446_HDR_BYTES + P446_MAX_REC*sizeof(struct p446_rec) > P446_RAM_BYTES);
    p446_image=vzalloc(P446_RAM_BYTES); if(!p446_image)return 0;
    p446_ram=ioremap_cache(P446_RAM_PHYS,P446_RAM_BYTES); p446_mdp=ioremap(P446_MDP_PHYS,P446_MDP_BYTES); p446_vbif=ioremap(P446_VBIF_PHYS,P446_VBIF_BYTES);
    p446_dispcc=ioremap(P446_DISPCC_PHYS,P446_DISPCC_BYTES); p446_gcc=ioremap(P446_GCC_PHYS,P446_GCC_BYTES); p446_refgen=ioremap(P446_REFGEN_PHYS,P446_REFGEN_BYTES); p446_phy=ioremap(P446_PHY_PHYS,P446_PHY_BYTES);
    p446_tlmm23=ioremap(0x0F117000ULL,0x10U);
    BUILD_BUG_ON(sizeof(struct p446_rec) != 672U);
    h=p446_hdr(); h->magic=P446_MAGIC; h->version=P446_VERSION; h->record_bytes=sizeof(struct p446_rec); h->capacity=P446_MAX_REC; h->boot_id=get_random_u64(); h->init_ns=ktime_get_boottime_ns(); p446_start_ns=h->init_ns; p446_sync_header();
    a52_p446_mark(0U,0,0); return 0;
}
pure_initcall(p446_early_init);
static int p446_burst_threadfn(void *unused)
{
	/* A52_U2_AB_OBSERVER_QUIET_V1 */
	static bool a52_u2_ab_disable_p446_burst_threadfn = true;
	if (!READ_ONCE(a52_u2_ab_disable_p446_burst_threadfn)) {

    u64 deadline;
    (void)unused;
    while (!kthread_should_stop()) {
        wait_event_interruptible(p446_burst_wq,
            kthread_should_stop() || atomic_read(&p446_burst));
        if (kthread_should_stop())
            break;
        deadline = ktime_get_boottime_ns() + 120000000ULL;
        while (!kthread_should_stop() && atomic_read(&p446_burst)) {
            a52_p446_mark(P446_EVT_PERIODIC, 1U, 0U);
            if (ktime_get_boottime_ns() >= deadline) {
                atomic_set(&p446_burst, 0);
                a52_p446_mark(0x181U, 1U, 120U);
                break;
            }
            usleep_range(900, 1100);
        }
    }
    return 0;

	}
	return 0;
}
void a52_p446_set_burst(u32 enable)
{
    atomic_set(&p446_burst, !!enable);
    a52_p446_mark(0x180U, !!enable, 0U);
    if (enable)
        wake_up_interruptible(&p446_burst_wq);
}
EXPORT_SYMBOL_GPL(a52_p446_set_burst);
static int __init p446_sampler_init(void)
{
    p446_burst_task = kthread_run(p446_burst_threadfn, NULL, "p446-burst");
    if (IS_ERR(p446_burst_task))
        p446_burst_task = NULL;
    mod_delayed_work(system_unbound_wq, &p446_work, 0);
    return 0;
}

subsys_initcall_sync(p446_sampler_init);
static ssize_t p446_raw_read(struct file *f,char __user *b,size_t c,loff_t *p){ if(!p446_image)return -ENODATA; return simple_read_from_buffer(b,c,p,p446_image,P446_RAM_BYTES); }
static int p446_show(struct seq_file *m,void *v){ struct p446_header *h=p446_hdr(); seq_printf(m,"P446 n=%u drop=%u rec=%u cb2=%u cb=%u first=%llu last=%llu crc=%08x\\n",h?h->count:0,h?h->dropped:0,h?h->record_bytes:0,h?h->cb2_param:0,h?h->actual_cb:~0U,h?(unsigned long long)h->first_ns:0,h?(unsigned long long)h->last_ns:0,h?h->hdr_crc32:0); return 0; }
static int p446_open(struct inode *i,struct file *f){ return single_open(f,p446_show,NULL); }
static const struct proc_ops p446_raw_ops={.proc_read=p446_raw_read,.proc_lseek=default_llseek};
static const struct proc_ops p446_ops={.proc_open=p446_open,.proc_read=seq_read,.proc_lseek=seq_lseek,.proc_release=single_release};
static int __init p446_proc_init(void){ proc_create("a52_phase446_raw",0444,NULL,&p446_raw_ops); proc_create("a52_phase446",0444,NULL,&p446_ops); return 0; }
late_initcall(p446_proc_init);
static const char p446_marker[] __used = "A52_PHASE446_EARLY_SPLASH_HEARTBEAT_V1";
static const char p446d_marker[] __used = "A52_PHASE446D_POWER_CLOCK_MICROSCOPE_V1:GKI";
static const char p446g_marker[] __used = "A52_PHASE446G_SKIP_TE_IRQ_TLMM23_V1";
static const char p446h_marker[] __used = "A52_PHASE446H_PANEL_F0_LIFECYCLE_V1";
static const char p446b_marker[] __used = "A52_PHASE446B_GATE_CB_REFRESH_V1";
static const char p446b_micro_marker[] __used = "A52_PHASE446B_FIRST_COMMIT_MICROSCOPE_V1";

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * P480 A3c: private Samsung SM7325 5.4 APR/AFE ABI extensions.
 * Donor: turex/android_kernel_samsung_sm7325 at
 * c4cb0a9783fcfb8d17ddda02c0ad1678dcd0dac4.
 *
 * Keep these packet IDs and payload structures private to the native
 * A3c DSP integration. Do not rewrite the original P479 APR/UAPI.
 */
#ifndef P480_A3C_DSP_ABI_H
#define P480_A3C_DSP_ABI_H

#include <linux/types.h>

#ifndef AFE_PARAM_ID_CODEC_DMA_DATA_ALIGN
#define AFE_PARAM_ID_CODEC_DMA_DATA_ALIGN 0x000102EA
#endif
#ifndef AFE_PARAM_ID_PORT_DATA_LOGGING_DISABLE
#define AFE_PARAM_ID_PORT_DATA_LOGGING_DISABLE 0x000102E9
#endif
#ifndef AVS_PARAM_ID_PEER_MTU_ID
#define AVS_PARAM_ID_PEER_MTU_ID 0x0001F101
#endif
#ifndef AVS_PARAM_ID_A2DP_SUSPEND_ID
#define AVS_PARAM_ID_A2DP_SUSPEND_ID 0x0001F107
#endif
#ifndef AVS_PARAM_ID_ENC_FORMAT_ID
#define AVS_PARAM_ID_ENC_FORMAT_ID 0x0001F105
#endif
#ifndef AVS_MEDIA_ID_SBM_ID
#define AVS_MEDIA_ID_SBM_ID 0x00010C19
#endif
#ifndef CAPI_V2_PARAM_SET_LC3_ENC_DOWNMIX_2_MONO
#define CAPI_V2_PARAM_SET_LC3_ENC_DOWNMIX_2_MONO 0x00013384
#endif
#ifndef ASM_MEDIA_FMT_LC3
#define ASM_MEDIA_FMT_LC3 0x0001337E
#endif

/* These payload layouts are copied from the pinned donor
 * techpack/audio/include/dsp/apr_audio-v2.h.
 */
struct afe_param_id_cdc_dma_data_align {
	u32 cdc_dma_data_align;
} __packed;

struct afe_param_id_port_data_log_disable_t {
	u32 disable_logging_flag;
} __packed;

struct asm_sbm_param_t {
	u32 target_delay;
	u32 prev_delay;
	u32 current_buffer_level;
	u32 stop_flag;
} __packed;

struct asm_mtu_param_t {
	u32 mtu;
} __packed;

struct asm_a2dp_suspend_param_t {
	u32 a2dp_suspend;
} __packed;

struct asm_enc_format_param_t {
	u32 enc_format;
} __packed;

struct lc3_channel_mode_param_t {
	u32 channel_mode;
} __packed;

#endif /* P480_A3C_DSP_ABI_H */

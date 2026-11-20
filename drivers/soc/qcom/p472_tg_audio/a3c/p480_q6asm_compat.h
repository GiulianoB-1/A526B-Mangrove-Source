/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * P480: Samsung SM7325 ASoC callers vs existing TG A3a Q6ASM ABI.
 *
 * Keep the real TG Q6ASM implementation. Its PCM v5 API has 7
 * arguments, not the donor's 9, and its render API has 3, not 2.
 * A direct call would misplace arguments on arm64 while still linking.
 * Reject custom maps instead of pretending they reached the DSP.
 */
#ifndef P480_Q6ASM_COMPAT_H
#define P480_Q6ASM_COMPAT_H

#include <linux/errno.h>
#include <linux/kernel.h>

extern int p480_tg_pcm_format_v5(struct audio_client *ac,
    uint32_t rate, uint32_t channels, uint16_t bits_per_sample,
    uint16_t sample_word_size, uint16_t endianness, uint16_t mode)
    __asm__("q6asm_enc_cfg_blk_pcm_format_support_v5");

static inline int p480_native_pcm_format_v5(struct audio_client *ac,
    uint32_t rate, uint32_t channels, bool use_default_chmap,
    char *channel_map, uint16_t bits_per_sample,
    uint16_t sample_word_size, uint16_t endianness, uint16_t mode)
{
    if (!use_default_chmap) {
        pr_warn_ratelimited("P480 A3c custom PCM channel map not ported\n");
        return -EOPNOTSUPP;
    }

    return p480_tg_pcm_format_v5(ac, rate, channels,
        bits_per_sample, sample_word_size, endianness, mode);
}

extern int p480_tg_render_mode(struct audio_client *ac,
    uint32_t render_mode, int dir)
    __asm__("q6asm_send_mtmx_strtr_render_mode");

static inline int p480_native_render_mode(struct audio_client *ac,
    uint32_t render_mode)
{
    /* The Samsung 5.4 implementation always sends direction RX (0). */
    return p480_tg_render_mode(ac, render_mode, 0);
}

/* Map Samsung A3c calls only after the donor function declarations. */
#define q6asm_enc_cfg_blk_pcm_format_support_v5 p480_native_pcm_format_v5
#define q6asm_send_mtmx_strtr_render_mode p480_native_render_mode

#endif /* P480_Q6ASM_COMPAT_H */

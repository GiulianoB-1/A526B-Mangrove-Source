/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_P481_AUDIO_DIAG_H
#define _LINUX_P481_AUDIO_DIAG_H
#include <linux/types.h>
#include <linux/device.h>
struct snd_soc_card;
#ifdef CONFIG_P480_A3C
bool p481_card_armed(void);
bool p481_mbhc_disabled(void);
bool p481_bias_hold(void);
bool p481_kona_card(const struct snd_soc_card *card);
void p481_alive_start(void);
void p481_rpm_forbid_device(struct device *dev);
int p481_diag_register(void);
void p483_card_auto_start(void);
void p486_card_adsp_up(void);
unsigned int p483_bolero_guard(void);
#else
static inline bool p481_card_armed(void) { return false; }
static inline bool p481_mbhc_disabled(void) { return false; }
static inline bool p481_bias_hold(void) { return false; }
static inline void p481_rpm_forbid_device(struct device *dev) {}
#endif
#endif

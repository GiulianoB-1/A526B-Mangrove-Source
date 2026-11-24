/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_P484_AUDIO_H
#define _LINUX_P484_AUDIO_H
#include <linux/device.h>
struct snd_soc_component;
struct swr_master;
struct wcd_mbhc;
int p484_cdc_vote_get(struct device *bolero, unsigned int port);
void p484_cdc_vote_put(struct device *bolero, unsigned int port);
bool p484_cdc_vote_held(struct device *bolero);
int p484_swr_dump_get(struct swr_master *master);
void p484_swr_dump_put(struct swr_master *master);
int p484_mbhc_dump_start(struct wcd_mbhc *mbhc);
int p484_mbhc_dump_knob(void);
void p484_mbhc_dump_clear(struct snd_soc_component *component);
#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_A52_AUTOSTART_H
#define _LINUX_A52_AUTOSTART_H
#include <linux/types.h>
struct rproc;
int a52_p487_scm_enable_once(void);
int a52_p487_ipa_arm_once(bool automatic);
#ifdef CONFIG_P480_A3C
bool a52_p487_autostart_enabled(void);
bool a52_p487_is_adsp(struct rproc *rproc);
struct rproc *a52_p487_find_adsp(void);
int a52_p487_adsp_start_once(struct rproc *rproc);
#endif
#endif

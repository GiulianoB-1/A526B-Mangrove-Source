/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_A52_IPA_ARM_H
#define _LINUX_A52_IPA_ARM_H
/* Real completion from the pinned IPA post-init path; no fake ready. */
int a52_p470_ipa_wait_ready(unsigned long timeout_jiffies);
#endif

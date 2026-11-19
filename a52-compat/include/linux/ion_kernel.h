/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __A52_COMPAT_LINUX_ION_KERNEL_H__
#define __A52_COMPAT_LINUX_ION_KERNEL_H__
/* A52_PHASE14_REAL_ION_RUNTIME: use ACK 5.10 ION, never ENODEV stubs. */
#include <linux/ion.h>
#ifndef ION_FLAG_CACHED
#define ION_FLAG_CACHED 1
#endif
#endif

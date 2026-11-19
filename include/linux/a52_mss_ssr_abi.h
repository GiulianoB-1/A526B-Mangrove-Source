/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_A52_MSS_SSR_ABI_H
#define _LINUX_A52_MSS_SSR_ABI_H
#include <linux/stddef.h>

/* Intentionally independent of struct subsys_desc definition. */
struct subsys_desc;
struct a52_ssr_desc_abi {
    size_t size;
    size_t name;
    size_t dev;
    size_t owner;
    size_t powerup;
    size_t shutdown;
    size_t state;
    size_t force_stop_bit;
    size_t err_fatal_irq;
};
struct subsys_desc *a52_p470_ssr_desc_alloc(const struct a52_ssr_desc_abi *caller);
void a52_p470_ssr_desc_free(struct subsys_desc *desc);
#endif

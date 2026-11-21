/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * P480 A3c: guard AFE real-time proxy port ID ABI.
 *
 * Samsung 5.4 callers pass four args, including port ID, but
 * the P479/TG AFE function accepts three and always targets
 * RT_PROXY_PORT_001_RX. Passing any other ID to that function
 * silently routes data to the wrong physical DSP port.
 */
#ifndef P480_AFE_PROXY_COMPAT_H
#define P480_AFE_PROXY_COMPAT_H

#include <linux/errno.h>
#include <linux/kernel.h>

extern int p480_tg_rt_proxy_port_read(phys_addr_t buf_addr_p,
    u32 mem_map_handle, int bytes)
    __asm__("afe_rt_proxy_port_read");

static inline int p480_native_rt_proxy_port_read(phys_addr_t buf_addr_p,
    u32 mem_map_handle, int bytes, int id)
{
    if (id != RT_PROXY_PORT_001_RX) {
        pr_warn_ratelimited("P480 A3c unsupported AFE proxy read port=0x%x\n", id);
        return -EOPNOTSUPP;
    }

    return p480_tg_rt_proxy_port_read(buf_addr_p, mem_map_handle, bytes);
}

/* Applied only in the A3c provider, after the 5.4 prototype. */
#define afe_rt_proxy_port_read p480_native_rt_proxy_port_read

#endif /* P480_AFE_PROXY_COMPAT_H */

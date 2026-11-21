/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * P480 native ASoC routing bridge for real TouchGrass 4.19 ADM.
 * The selected Samsung SM7325 5.4 routing code passes an additional
 * COPP token and optionally per-session echo-reference configuration.
 * Existing P479 ADM handles the 11-argument base open only.
 *
 * Do not silently discard nonzero tokens or echo-reference data.
 * Until the 5.4 ADM extension is fully ported, fail these cases closed.
 * Regular token-free RX/TX routes still use genuine A3a ADM calls.
 */
#ifndef P480_NATIVE_ADM_COMPAT_H
#define P480_NATIVE_ADM_COMPAT_H

#include <linux/errno.h>
#include <linux/kernel.h>

/* Bind to the unchanged legacy ADM function with its exact call ABI. */
extern int p480_tg_adm_open(int port, int path, int rate, int mode,
			 int topology, int perf_mode, uint16_t bits_per_sample,
			 int app_type, int acdbdev_id, int session_type,
			 uint32_t pass_thr) __asm__("adm_open");

static inline int p480_native_adm_open(int port, int path, int rate,
		int mode, int topology, int perf_mode,
		uint16_t bits_per_sample, int app_type, int acdbdev_id,
		int session_type, uint32_t pass_thr, uint32_t copp_token)
{
	if (copp_token) {
		pr_warn_ratelimited("P480 A3c unsupported ADM copp_token=0x%x\\n",
			copp_token);
		return -EOPNOTSUPP;
	}
	return p480_tg_adm_open(port, path, rate, mode, topology,
		perf_mode, bits_per_sample, app_type, acdbdev_id,
		session_type, pass_thr);
}

static inline int p480_native_adm_open_v2(int port, int path, int rate,
		int mode, int topology, int perf_mode,
		uint16_t bits_per_sample, int app_type, int acdbdev_id,
		int session_type, uint32_t pass_thr, uint32_t copp_token,
		struct msm_ec_ref_port_cfg *ec_ref_port_cfg,
		struct msm_pcm_channel_mixer *ec_ref_chmix_cfg)
{
	if (ec_ref_port_cfg || ec_ref_chmix_cfg) {
		pr_warn_ratelimited("P480 A3c per-session ADM echo-reference not yet ported\\n");
		return -EOPNOTSUPP;
	}
	return p480_native_adm_open(port, path, rate, mode,
		topology, perf_mode, bits_per_sample, app_type,
		acdbdev_id, session_type, pass_thr, copp_token);
}

/* The 5.4 matrix mapper adds a frontend ID used in its calibration.
 * The older A3a implementation has no per-FE calibration input. Reject
 * non-default frontends rather than silently shifting the C ABI.
 */
extern int p480_tg_adm_matrix_map(int path, struct route_payload payload_map,
		int perf_mode, uint32_t passthr_mode)
		__asm__("adm_matrix_map");

static inline int p480_native_adm_matrix_map(int fedai_id, int path,
		struct route_payload payload_map, int perf_mode,
		uint32_t passthr_mode)
{
	if (fedai_id != 0) {
		pr_warn_ratelimited("P480 A3c FE-specific ADM calibration unsupported (FE=%d)\\n", fedai_id);
		return -EOPNOTSUPP;
	}
	return p480_tg_adm_matrix_map(path, payload_map, perf_mode,
		passthr_mode);
}

/* Applied after the genuine 5.4 declaration has been parsed. */
#define adm_open p480_native_adm_open
#define adm_open_v2 p480_native_adm_open_v2
#define adm_matrix_map p480_native_adm_matrix_map

#endif /* P480_NATIVE_ADM_COMPAT_H */

/* SPDX-License-Identifier: GPL-2.0-only */
/* A52_PHASE447_GATES_V1; no port implied. */
#ifndef _LINUX_A52_PHASE447_H
#define _LINUX_A52_PHASE447_H
#include <linux/types.h>
#include <linux/device.h>
enum a52_447_subsys {
	A52_447_SM5714,
	A52_447_DIRECTCHARGER,
	A52_447_BATTERY,
	A52_447_CHARGER,
	A52_447_FUELGAUGE,
	A52_447_MUIC,
	A52_447_PDIC,
	A52_447_ADC,
	A52_447_AOSS,
	A52_447_GLINK,
	A52_447_GLINKPKT,
	A52_447_SSRCOMPAT,
	A52_447_RMTFSCOMPAT,
	A52_447_ADSP,
	A52_447_CDSP,
	A52_447_MSS,
	A52_447_IPA,
	A52_447_AUDIO,
	A52_447_AUDIOAPR,
	A52_447_CODEC,
	A52_447_SENSORS,
	A52_447_FINGERPRINT,
	A52_447_NFC,
	A52_447_VIBRATOR,
	A52_447_WIFI,
	A52_447_BT,
	A52_447_VIDC,
	A52_447_CAMERA,
	A52_447_DEVBW,
	A52_447_SUSPEND,
	A52_447_THERMAL,
	A52_447_MISC,
	A52_447_COUNT
};
bool a52_447_enabled(enum a52_447_subsys id);
bool a52_447c_trace_verbose(void);
bool a52_diag_enabled(void);
/* Only for diagnostic printk calls, never for hardware/state operations. */
#define a52_diag_log_if(keep, log, ...) do { \
	if ((keep) || a52_diag_enabled()) \
		log(__VA_ARGS__); \
} while (0)
int a52_447_probe_allowed(struct device *dev, enum a52_447_subsys id);
#endif

// SPDX-License-Identifier: GPL-2.0-only
/* A52_PHASE447_GATES_V1 -- Phase447C defaults compiled from .config */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kconfig.h>
#include <linux/a52_phase447.h>
#if !defined(CONFIG_A52_WAVE_DEFAULT)
#error "P447C requires compiled CONFIG_A52_WAVE_DEFAULT"
#endif
static unsigned int a52_447_wave=CONFIG_A52_WAVE_DEFAULT;
static bool a52_447_source_cmdline;
static bool a52_447c_verbose;
static bool a52_diag_verbose;
bool a52_diag_enabled(void)
{
	return READ_ONCE(a52_diag_verbose);
}
EXPORT_SYMBOL_GPL(a52_diag_enabled);
static int __init a52_diag_parse(char *v)
{
	if (!v || kstrtobool(v, &a52_diag_verbose))
		return -EINVAL;
	return 0;
}
early_param("a52.diag", a52_diag_parse);
bool a52_447c_trace_verbose(void)
{
	return READ_ONCE(a52_447c_verbose) || a52_diag_enabled();
}
EXPORT_SYMBOL_GPL(a52_447c_trace_verbose);
static int __init a52_447c_parse_trace(char *v)
{
	unsigned int n;
	if (!v || kstrtouint(v,0,&n) || n>1) return -EINVAL;
	a52_447c_verbose=n!=0;
	return 0;
}
early_param("a52.trace_verbose",a52_447c_parse_trace);
static int __init a52_447_parse_wave(char *v)
{
	unsigned int n;
	if (!v || kstrtouint(v,0,&n) || n<1 || n>8) return -EINVAL;
	a52_447_wave=n;
	a52_447_source_cmdline=true;
	return 0;
}
early_param("a52.wave",a52_447_parse_wave);
#define GATE_DEFAULT(name,initial) \
	static int a52_447_override_##name=(initial); \
	static int __init a52_447_parse_##name(char *v) \
	{ \
		unsigned int n; \
		if (!v || kstrtouint(v,0,&n) || n>1) return -EINVAL; \
		a52_447_override_##name=n; \
		a52_447_source_cmdline=true; \
		return 0; \
	} \
	early_param("a52." #name,a52_447_parse_##name);
#define GATE(name) GATE_DEFAULT(name,-1)
GATE(sm5714)
GATE(directcharger)
GATE(battery)
GATE(charger)
GATE(fuelgauge)
GATE(muic)
GATE(pdic)
GATE(adc)
GATE(aoss)
GATE(glink)
GATE(glinkpkt)
GATE(ssrcompat)
GATE(rmtfscompat)
GATE(adsp)
GATE(cdsp)
GATE_DEFAULT(mss,IS_ENABLED(CONFIG_A52_MSS_DEFAULT))
GATE_DEFAULT(ipa,IS_ENABLED(CONFIG_A52_IPA_DEFAULT))
GATE(audio)
GATE(audioapr)
GATE(codec)
GATE(sensors)
GATE(fingerprint)
GATE(nfc)
GATE(vibrator)
GATE(wifi)
GATE(bt)
GATE(vidc)
GATE(camera)
GATE(devbw)
GATE(suspend)
GATE(thermal)
GATE(misc)
struct a52_447_gate { const char *name; unsigned int wave; int *override; };
static struct a52_447_gate gates[A52_447_COUNT] = {
	[A52_447_SM5714] = {"sm5714",2,&a52_447_override_sm5714},
	[A52_447_DIRECTCHARGER] = {"directcharger",2,&a52_447_override_directcharger},
	[A52_447_BATTERY] = {"battery",2,&a52_447_override_battery},
	[A52_447_CHARGER] = {"charger",2,&a52_447_override_charger},
	[A52_447_FUELGAUGE] = {"fuelgauge",2,&a52_447_override_fuelgauge},
	[A52_447_MUIC] = {"muic",2,&a52_447_override_muic},
	[A52_447_PDIC] = {"pdic",2,&a52_447_override_pdic},
	[A52_447_ADC] = {"adc",2,&a52_447_override_adc},
	[A52_447_AOSS] = {"aoss",3,&a52_447_override_aoss},
	[A52_447_GLINK] = {"glink",3,&a52_447_override_glink},
	[A52_447_GLINKPKT] = {"glinkpkt",3,&a52_447_override_glinkpkt},
	[A52_447_SSRCOMPAT] = {"ssrcompat",3,&a52_447_override_ssrcompat},
	[A52_447_RMTFSCOMPAT] = {"rmtfscompat",3,&a52_447_override_rmtfscompat},
	[A52_447_ADSP] = {"adsp",3,&a52_447_override_adsp},
	[A52_447_CDSP] = {"cdsp",3,&a52_447_override_cdsp},
	[A52_447_MSS] = {"mss",3,&a52_447_override_mss},
	[A52_447_IPA] = {"ipa",3,&a52_447_override_ipa},
	[A52_447_AUDIO] = {"audio",4,&a52_447_override_audio},
	[A52_447_AUDIOAPR] = {"audioapr",4,&a52_447_override_audioapr},
	[A52_447_CODEC] = {"codec",4,&a52_447_override_codec},
	[A52_447_SENSORS] = {"sensors",5,&a52_447_override_sensors},
	[A52_447_FINGERPRINT] = {"fingerprint",5,&a52_447_override_fingerprint},
	[A52_447_NFC] = {"nfc",5,&a52_447_override_nfc},
	[A52_447_VIBRATOR] = {"vibrator",5,&a52_447_override_vibrator},
	[A52_447_WIFI] = {"wifi",6,&a52_447_override_wifi},
	[A52_447_BT] = {"bt",6,&a52_447_override_bt},
	[A52_447_VIDC] = {"vidc",7,&a52_447_override_vidc},
	[A52_447_CAMERA] = {"camera",7,&a52_447_override_camera},
	[A52_447_DEVBW] = {"devbw",8,&a52_447_override_devbw},
	[A52_447_SUSPEND] = {"suspend",8,&a52_447_override_suspend},
	[A52_447_THERMAL] = {"thermal",8,&a52_447_override_thermal},
	[A52_447_MISC] = {"misc",8,&a52_447_override_misc},
};
bool a52_447_enabled(enum a52_447_subsys id)
{
	if (id < 0 || id >= A52_447_COUNT) return false;
	if (*gates[id].override == 0) return false;
	if (*gates[id].override > 0) return true;
	return a52_447_wave >= gates[id].wave;
}
EXPORT_SYMBOL_GPL(a52_447_enabled);
int a52_447_probe_allowed(struct device *dev, enum a52_447_subsys id)
{
	if (id < 0 || id >= A52_447_COUNT) return -EINVAL;
	if (a52_447_enabled(id)) return 0;
	if (dev) dev_info(dev,"P447 W%u %s DISABLED (a52.%s=0)\n",
			gates[id].wave,gates[id].name,gates[id].name);
	return -ENODEV;
}
EXPORT_SYMBOL_GPL(a52_447_probe_allowed);
static int __init a52_447_gates_init(void)
{
	int i;
	pr_info("P447 GATES source=%s wave=%u mss=%d ipa=%d new_drivers_only=1\n",
		a52_447_source_cmdline?"cmdline":"kconfig",a52_447_wave,
		a52_447_enabled(A52_447_MSS),a52_447_enabled(A52_447_IPA));
	pr_info("P447 GATES initialized wave=%u new_drivers_only=1\n",a52_447_wave);
	for (i=0;i<A52_447_COUNT;i++)
		pr_info("P447 W%u %s %s override=%d\n",gates[i].wave,
			gates[i].name,a52_447_enabled(i)?"ARMED":"DISABLED",
			*gates[i].override);
	return 0;
}
subsys_initcall(a52_447_gates_init);

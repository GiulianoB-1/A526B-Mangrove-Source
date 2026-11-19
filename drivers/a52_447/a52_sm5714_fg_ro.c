#include <linux/sched.h>
#include <linux/device.h>
// SPDX-License-Identifier: GPL-2.0-only
/* A52_PHASE447D_SM5714_FG_RO_V1:
 * Transitional live-battery monitor with no charging or flash controls.
 * Layout from TG 6bf351bd sm5714-private.h and sm5714_fuelgauge.c.
 */
#include <linux/i2c.h>
#include <linux/suspend.h>
#include <linux/notifier.h>
#include <linux/pm_wakeup.h>
#if IS_ENABLED(CONFIG_MGBATT_SHADOW)
#include "mgbatt_shadow/mgbatt_shadow.h"
#endif
#include <linux/iio/consumer.h>
#include <linux/math64.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/kobject.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/a52_phase447.h>

#define A52D_SM5714_FG_ADDR 0x71
#define A52D_FG_RADDR 0x8c
#define A52D_FG_RDATA 0x8d
#define A52D_FG_SOC 0x00
#define A52D_FG_VBAT 0x03
#define A52D_FG_TEMP 0x07
#define A52D_FG_ID 0x00

struct a52d_fg {
	u32 a52f_capacity_max;
	u32 a52f_capacity_min;
	u32 a52f_calc_type;
	struct platform_device *a52f_fg_child;
	struct device *dev;
	struct i2c_client *fg;
	struct power_supply *psy;
	struct mutex lock;
	struct delayed_work snapshot;
	struct delayed_work a52g_guard_work;
    bool p476_h2_enabled, p476_h2_fast;
#if IS_ENABLED(CONFIG_P479_H4)
    bool p479_h4_auto,p479_h4_ready,p479_h4_faulted;
    bool p479_h4_rearm_allowed,p479_h4_saw_unplug;
    const char *p479_h4_pending,*p479_h4_type;
    unsigned long p479_h4_seen;
    int p479_h4_applied_icl,p479_h4_applied_fcc;
#endif
#if IS_ENABLED(CONFIG_P477_H3)
    bool p477_h3_bat_hot;
    int p477_h3_target_ma, p477_h3_actual_ma;
    int p477_h3_bat_anchor, p477_h3_chg_anchor;
    unsigned long p477_h3_stable_since, p477_h3_last_step;
#endif
    int p475_s2_last_dt1;
    const char *p475_s2_last_type;
    struct notifier_block a52g_pm_nb;
    bool a52g_pm_registered, a52g_pm_paused;
    unsigned long a52g_pm_grace_until;
    struct mutex a52h0_lock;
    bool a52h0_armed, a52h0_active, a52h0_killed, a52h0_full;
    bool a52h0_rearm, a52h0_ntc_valid, a52h0_timer_started;
    const char *a52h0_state;
    unsigned long a52h0_ntc_since;
    int a52h0_id;
    struct power_supply *a52h0_usb;
    struct wakeup_source *a52h0_ws;
    bool a52h0_wake_held;
    unsigned long a52h0_started, a52h0_last_kick;
    unsigned int a52h0_verify_io_streak;
	struct i2c_client *a52g_chg;
	struct mutex a52g_ntc_lock;
	struct iio_channel *a52g_ntc;
	struct iio_channel *a52h_chg_iio;
	int a52h_chg_points;
	int a52h_chg_adc[32];
	int a52h_chg_deciC[32];
	int a52g_ntc_points;
	int a52g_ntc_adc[32];
	int a52g_ntc_deciC[32];
	u32 a52g_guard_cycle;
    int a52p451_last_vbus;
    unsigned long a52p450_started;
	bool a52g_inhibit_latched;
	bool reported_first;
};
/* This selector changes which SRAM location is read, not any learned/NV
 * parameter. No call is made to 0x8e/0x8f (SRAM WADDR/WDATA) or OTP.
 */
static int a52d_sram_read(struct a52d_fg *fg, u8 address)
{
	int rc;
	mutex_lock(&fg->lock);
	rc=i2c_smbus_write_word_data(fg->fg,A52D_FG_RADDR,address);
	if (rc>=0)
		rc=i2c_smbus_read_word_data(fg->fg,A52D_FG_RDATA);
	mutex_unlock(&fg->lock);
	return rc;
}

/* A52_PHASE447F_SOC_PARITY_CORRECTED_V2
 * TG sm5714_get_soc(): 0.1% = (raw_sram_word * 10) >> 8.
 * Preserve the GKI capacity rounding which was independently verified.
 */
static int a52f_soc_deci(struct a52d_fg *fg)
{
	int v=a52d_sram_read(fg,A52D_FG_SOC),soc;
	if (v<0)return v;
	soc=(v*10)>>8;
	if (soc<0 || soc>1100)return -ERANGE;
	return clamp_val(soc,0,1000);
}
static int a52d_soc(struct a52d_fg *fg)
{
	int deci=a52f_soc_deci(fg);
	if (deci<0)return deci;
	return clamp_val((deci+5)/10,0,100);
}
static int a52d_mv(struct a52d_fg *fg)
{
	int v=a52d_sram_read(fg,A52D_FG_VBAT),mv;
	if (v<0) return v;
	mv=2700+((v & 0x8000) ? -1 : 1)*((v&0x7fff)*10/109);
	if (mv<2500 || mv>5000) return -ERANGE;
	return mv;
}
static int a52d_temp(struct a52d_fg *fg)
{
	int v=a52d_sram_read(fg,A52D_FG_TEMP),t;
	/* Valid negative Celsius readings exist. Use an out-of-range error
	 * sentinel so probe never mistakes -EIO for -5 deci-Celsius.
	 */
	if (v<0) return -10000;
	t=((v&0x7fff)*10*2989)>>19;
	if (v&0x8000)t=-t;
	if (t < -400 || t>1100)return -10000;
	return t;
}


/* A52_PHASE447G_CHARGER_GUARD_V1
 * WARNING: backup thermal inhibit only. FG is NOT the verified battery
 * NTC. A full charger/thermistor/JEITA state machine remains necessary.
 * TG sm5714_charger.c chg_set_enq4fet(FALSE) clears CNTL1 bit 3.
 * Primary I2C client is SM5714 charger 0x49. FG at 0x71 is read-only.
 */
#define A52G_CHG_STATUS1 0x0d
#define A52G_CHG_CNTL1 0x13
#define A52G_CHG_DEVICEID 0x50
#define A52G_CHG_ENQ4FET BIT(3)
#define A52G_GUARD_TEMP_DECI_C 450
#define A52G_GUARD_POLL_MS 2000


/* A52_PHASE447G_BATT_THERM_NTC_V1
 * Exact read-only A52 R02 io-channel "adc-temp" + Samsung temperature table.
 * A failed/missing IIO ADC must fall back to the FG safety thermometer.
 */
#define A52G_NTC_MAX_POINTS 32
static int a52g_ntc_temp(struct a52d_fg *fg)
{
	int raw,rc,j,t=-10000;
	mutex_lock(&fg->a52g_ntc_lock);
	if (!fg->a52g_ntc || fg->a52g_ntc_points<2)
		goto out;
	rc=iio_read_channel_processed(fg->a52g_ntc,&raw);
	if (rc<0 || raw<0 || raw>2000000)
		goto out;
	if (raw<=fg->a52g_ntc_adc[0]) {
		t=fg->a52g_ntc_deciC[0];
		goto out;
	}
	for (j=1;j<fg->a52g_ntc_points;j++) {
		int x0=fg->a52g_ntc_adc[j-1],x1=fg->a52g_ntc_adc[j];
		int y0=fg->a52g_ntc_deciC[j-1],y1=fg->a52g_ntc_deciC[j];
		if (x1<=x0)
			goto out;
		if (raw>x1)
			continue;
		t=y0+(int)div_s64((s64)(raw-x0)*(y1-y0),x1-x0);
		goto out;
	}
	t=fg->a52g_ntc_deciC[fg->a52g_ntc_points-1];
out:
	mutex_unlock(&fg->a52g_ntc_lock);
	return t;
}
static void a52g_ntc_cleanup(void *data)
{
	struct a52d_fg *fg=data;
	struct iio_channel *ch;
	mutex_lock(&fg->a52g_ntc_lock);
	ch=fg->a52g_ntc;
	fg->a52g_ntc=NULL;
	fg->a52g_ntc_points=0;
	mutex_unlock(&fg->a52g_ntc_lock);
	if (ch)
		iio_channel_release(ch);
}
static int a52g_ntc_attach(struct platform_device *pdev,struct a52d_fg *fg)
{
	struct iio_channel *ch;
	u32 adc[A52G_NTC_MAX_POINTS],temps[A52G_NTC_MAX_POINTS];
	int points,i,rc;
	struct device_node *np=pdev->dev.of_node;
	if (!np)return -ENODEV;
	if (READ_ONCE(fg->a52g_ntc))return 0;
	points=of_property_count_u32_elems(np,"battery,temp_table_adc");
	if (points<2 || points>A52G_NTC_MAX_POINTS ||
	    points!=of_property_count_u32_elems(np,"battery,temp_table_data")) {
		dev_warn(&pdev->dev,
			"P447G CHG_GUARD THERM_NTC table_invalid count=%d fallback=FG\n",
			points);
		return -EINVAL;
	}
	rc=of_property_read_u32_array(np,"battery,temp_table_adc",adc,points);
	if (rc)return rc;
	rc=of_property_read_u32_array(np,"battery,temp_table_data",temps,points);
	if (rc)return rc;
	for (i=1;i<points;i++)
		if (adc[i]<=adc[i-1]) {
			dev_warn(&pdev->dev,
				"P447G CHG_GUARD THERM_NTC unsorted_table idx=%d fallback=FG\n",i);
			return -EINVAL;
		}
	/* Match TouchGrass sec_adc.c AP-channel lookup on the actual
	 * battery platform node; not an invented alias on the I2C client.
	 */
	ch=iio_channel_get(&pdev->dev,"adc-temp");
	if (IS_ERR(ch)) {
		dev_info(&pdev->dev,
			"P447G CHG_GUARD THERM_NTC adc-temp unavailable rc=%ld fallback=FG\n",
			PTR_ERR(ch));
		return PTR_ERR(ch);
	}
	mutex_lock(&fg->a52g_ntc_lock);
	for (i=0;i<points;i++) {
		fg->a52g_ntc_adc[i]=(int)adc[i];
		fg->a52g_ntc_deciC[i]=(int)temps[i];
	}
	fg->a52g_ntc_points=points;
	fg->a52g_ntc=ch;
	mutex_unlock(&fg->a52g_ntc_lock);
	rc=devm_add_action_or_reset(&pdev->dev,a52g_ntc_cleanup,fg);
	if (rc) {
		dev_err(&pdev->dev,
			"P447G CHG_GUARD THERM_NTC cleanup_registration rc=%d\n",rc);
		return rc;
	}
	dev_info(&pdev->dev,
		"P447G CHG_GUARD THERM_NTC source=BAT_THERM_IIO channel=adc-temp points=%d first_adc=%u last_adc=%u\n",
		points,adc[0],adc[points-1]);
	return 0;
}


/* A52_PHASE447H_TCHG_GPIO3_IIO_V1
 * TG battery dt channel "adc-chg-temp" => ADC5 GPIO3/0x54 VOLTAGE uV.
 * Samsung battery,chg_temp_table_adc/data convert uV to deciC.
 * Purely diagnostic; only bat_thm is controlling thermal guard.
 */
static int a52h_chg_ntc_temp(struct a52d_fg *fg)
{
	int raw,t=-10000,j,rc;
	mutex_lock(&fg->a52g_ntc_lock);
	if (!fg->a52h_chg_iio || fg->a52h_chg_points<2)
		goto out;
	rc=iio_read_channel_processed(fg->a52h_chg_iio,&raw);
	if (rc<0 || raw<0 || raw>2000000)
		goto out;
	if (raw<=fg->a52h_chg_adc[0]) {
		t=fg->a52h_chg_deciC[0];
		goto out;
	}
	for (j=1;j<fg->a52h_chg_points;j++) {
		int x0=fg->a52h_chg_adc[j-1],x1=fg->a52h_chg_adc[j];
		int y0=fg->a52h_chg_deciC[j-1],y1=fg->a52h_chg_deciC[j];
		if (x1<=x0)goto out;
		if (raw>x1)continue;
		t=y0+(int)div_s64((s64)(raw-x0)*(y1-y0),x1-x0);
		goto out;
	}
	t=fg->a52h_chg_deciC[fg->a52h_chg_points-1];
out:
	mutex_unlock(&fg->a52g_ntc_lock);
	return t;
}
static void a52h_chg_iio_cleanup(void *data)
{
	struct a52d_fg *fg=data;
	struct iio_channel *ch;
	mutex_lock(&fg->a52g_ntc_lock);
	ch=fg->a52h_chg_iio;
	fg->a52h_chg_iio=NULL;
	fg->a52h_chg_points=0;
	mutex_unlock(&fg->a52g_ntc_lock);
	if (ch)iio_channel_release(ch);
}
static int a52h_chg_ntc_attach(struct platform_device *pdev,struct a52d_fg *fg)
{
	struct iio_channel *ch;
	struct device_node *np=pdev->dev.of_node;
	u32 adc[32],temps[32];
	int n,i,rc;
	if (!np)return -ENODEV;
	if (READ_ONCE(fg->a52h_chg_iio))return 0;
	n=of_property_count_u32_elems(np,"battery,chg_temp_table_adc");
	if (n<2 || n>32 ||
	    n!=of_property_count_u32_elems(np,"battery,chg_temp_table_data"))
		return -EINVAL;
	rc=of_property_read_u32_array(np,"battery,chg_temp_table_adc",adc,n);
	if (rc)return rc;
	rc=of_property_read_u32_array(np,"battery,chg_temp_table_data",temps,n);
	if (rc)return rc;
	for (i=1;i<n;i++)
		if (adc[i]<=adc[i-1])return -EINVAL;
	ch=iio_channel_get(&pdev->dev,"adc-chg-temp");
	if (IS_ERR(ch))return PTR_ERR(ch);
	mutex_lock(&fg->a52g_ntc_lock);
	for (i=0;i<n;i++) {
		fg->a52h_chg_adc[i]=(int)adc[i];
		fg->a52h_chg_deciC[i]=(int)temps[i];
	}
	fg->a52h_chg_points=n;
	fg->a52h_chg_iio=ch;
	mutex_unlock(&fg->a52g_ntc_lock);
	rc=devm_add_action_or_reset(&pdev->dev,a52h_chg_iio_cleanup,fg);
	if (rc)return rc;
	dev_info(&pdev->dev,
		 "P447H TCHG READY src=adc-chg-temp channel=0x54 units=uv points=%d\n",n);
	return 0;
}


/* A52_PHASE450_DAILY_GUARD_FG_DIAG_V1
 * TG sm5714_fg_get_curr() reads SRAM 0x05/0x09; the signed value follows TG's 2.5x range and 0x8000 system-status
 * rules. The downstream flag_chg_status policy is NOT yet available.
 * Report uA, not guaranteed net charge/discharge classification.
 */
#define A52P450_FG_CURR 0x05
#define A52P450_FG_CURR_AVG 0x09
#define A52P450_FG_SYSTEM_STATUS 0x10
#define A52P450_FG_AUX_STAT 0x22
#define A52P450_CHG_STATUS2 0x0e
#define A52P450_CHG_STATUS3 0x0f
static int a52p450_read_current_ua(struct a52d_fg *fg, u8 addr,
                                   int *ua, int *aux_stat, int *sys_stat)
{
    int word=a52d_sram_read(fg,addr);
    int stat,aux,ma;
    if (word<0) return word;
    aux=i2c_smbus_read_word_data(fg->fg,A52P450_FG_AUX_STAT);
    if (aux<0) return aux;
    stat=i2c_smbus_read_word_data(fg->fg,A52P450_FG_SYSTEM_STATUS);
    if (stat<0) return stat;
    ma=((word&0x7fff)*1000)/2044;
    if (word&0x8000) ma=-ma;
    /* TG sm5714_get_curr: widened range and factory/init current zero. */
    if (aux&0x0400) ma=ma*25/10;
    if (stat&0x8000) ma=0;
    *ua=ma*1000;
    if (aux_stat) *aux_stat=aux;
    if (sys_stat) *sys_stat=stat;
    return 0;
}
static void a52p450_chg_state(struct a52d_fg *fg, const char *action)
{
    int ctrl=i2c_smbus_read_byte_data(fg->a52g_chg,A52G_CHG_CNTL1);
    int st1=i2c_smbus_read_byte_data(fg->a52g_chg,A52G_CHG_STATUS1);
    int st2=i2c_smbus_read_byte_data(fg->a52g_chg,A52P450_CHG_STATUS2);
    int st3=i2c_smbus_read_byte_data(fg->a52g_chg,A52P450_CHG_STATUS3);
    dev_info(fg->dev,
       "P450 CHG_STATE action=%s en=%d status1=%d status2=%d status3=%d charging=%d topoff=%d read_only=1\n",
       action,ctrl<0?-1:!!(ctrl&BIT(3)),st1,st2,st3,
       st2<0?-1:!!(st2&BIT(3)),st2<0?-1:!!(st2&BIT(5)));
}


/* A52_PHASE451_DAILY_READONLY_V1 — no writes: TG charger snapshot */
static void a52p451_chg_regs(struct a52d_fg *fg,const char *why,int vbus)
{
    static const u8 addresses[]={
        0x13,0x14,0x15,0x25,0x22,
        0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,
        0x1f,0x20,0x46,0x0d,0x0e,0x0f,0x10,0x11
    };
    int i,val;
    dev_info(fg->dev,"P451 CHG_REGS why=%s VBUS=%d begin read_only=1\n",
             why,vbus);
    for (i=0;i<ARRAY_SIZE(addresses);i++) {
        val=i2c_smbus_read_byte_data(fg->a52g_chg,addresses[i]);
        dev_info(fg->dev,"P451 CHG_REG addr=%#x val=%d rc=%d why=%s\n",
                 addresses[i],val,val<0?val:0,why);
    }
    dev_info(fg->dev,"P451 CHG_REGS why=%s end\n",why);
}

static int a52g_sample(struct a52d_fg *fg, bool first)
{
	int die,st,temp,fgtemp,ntc,chgtemp,ctrl,rc,readback,vbus;
	bool batt_trip,chg_trip,mix_trip,missing_chg;
	const char *rule;
	const char *source;
	bool unsafe;
	if (!IS_ENABLED(CONFIG_A52_447G_CHG_GUARD))
		return 0;
	die=i2c_smbus_read_byte_data(fg->a52g_chg,A52G_CHG_DEVICEID);
	if (die<0) {
		dev_err_ratelimited(fg->dev,
			"P447G CHG_GUARD device_id_read_failed rc=%d addr=%#x cannot_verify_inhibit\n",
			die,fg->a52g_chg->addr);
		return die;
	}
	fgtemp=a52d_temp(fg);
	ntc=a52g_ntc_temp(fg);
	source=(ntc>=-200 && ntc<=900)?"BATT_THERM":"FG_FALLBACK";
	temp=!strcmp(source,"BATT_THERM") ? ntc : fgtemp;
	st=i2c_smbus_read_byte_data(fg->a52g_chg,A52G_CHG_STATUS1);
	vbus=st>=0 ? !!(st & BIT(0)) : -1;
    if (first || vbus!=fg->a52p451_last_vbus ||
        fg->a52g_guard_cycle % 150 == 0)
        a52p451_chg_regs(fg,first?"BOOT_PRE_INHIBIT":
                        vbus!=fg->a52p451_last_vbus?"VBUS_CHANGED":"FIVE_MIN",vbus);
    fg->a52p451_last_vbus=vbus;
	/* No batt_therm channel is available while qcom adc5 rejects DT
	 * channel 83. FG is a conservative FALLBACK, not battery temperature.
	 * If its read fails with VBUS present, inhibit instead of guessing.
	 * A missing VBUS read is treated conservatively as potentially ON.
	 */
	/* A52_PHASE449F_CHG_MIX_GUARD_V1:
	 * TG live DT 0.1C: chg_high_temp=550 recovery=480,
	 * mix_high_temp=420 / mix_high_chg_temp=500,
	 * mix_high_temp_recovery=390.
	 * Until sec_battery exists this is a ONE-WAY INHIBIT, not throttle;
	 * do not automatically resume when the temperature falls.
	 * An unreadable charger thermistor with VBUS present also fails
	 * closed (guard first executes before the IIO attach retry).
	 */
	chgtemp=a52h_chg_ntc_temp(fg);
	batt_trip=(temp>=A52G_GUARD_TEMP_DECI_C || temp< -400);
	chg_trip=(chgtemp>=550);
	mix_trip=(temp>=420 && chgtemp>=500);
	missing_chg=(chgtemp< -400 || chgtemp>900);
	unsafe=batt_trip || chg_trip || mix_trip ||
		(missing_chg && vbus!=0);
	rule=batt_trip ? "batt" :
		chg_trip ? "chg" :
		mix_trip ? "mix" :
		(missing_chg && vbus!=0) ? "chg_sensor_missing" : "none";
	
    /* Initial Tchg IIO acquisition can lag the I2C FG probe.
     * Only defer the software latch while BOTH register indications
     * independently prove charging is already disabled. Otherwise
     * preserve P449's immediate fail-closed inhibit.
     * This never sets ENQ4FET and never resumes charging.
     */
    if (missing_chg && !batt_trip && !chg_trip && !mix_trip &&
        vbus>0 && !fg->a52g_inhibit_latched &&
        time_before(jiffies,fg->a52p450_started+10*HZ)) {
        int st2=i2c_smbus_read_byte_data(fg->a52g_chg,A52P450_CHG_STATUS2);
        int cntl=i2c_smbus_read_byte_data(fg->a52g_chg,A52G_CHG_CNTL1);
        if (st2>=0 && cntl>=0 &&
            !(st2&BIT(3)) && !(cntl&A52G_CHG_ENQ4FET)) {
            dev_info(fg->dev,
                "P450 CHG_GUARD sensor=NOT_READY action=WAIT charging=0 ctrl=%#x status2=%#x grace_ms=10000 never_enable=1\n",
                cntl,st2);
            a52p450_chg_state(fg,"WAIT_SENSOR");
            return 0;
        }
    }

    if (first || unsafe || fg->a52g_guard_cycle % 15 == 0)
		dev_info(fg->dev,
			"P449 CHG_GUARD rule=%s temp_batt=%d temp_chg=%d VBUS=%d action=%s latch=%d chg_limit=550 chg_recovery=480 mix_batt=420 mix_chg=500 mix_batt_recovery=390 never_reenable=1\n",
			rule,temp,chgtemp,vbus,unsafe && vbus!=0 ? "INHIBIT" : "MONITOR",
			fg->a52g_inhibit_latched);
	if (first || fg->a52g_guard_cycle % 15 == 0) {
        int now=0,avg=0,aux=-1,stat=-1;
        int now_rc=a52p450_read_current_ua(fg,A52P450_FG_CURR,
                                             &now,&aux,&stat);
        int avg_rc=a52p450_read_current_ua(fg,A52P450_FG_CURR_AVG,
                                             &avg,NULL,NULL);
        dev_info(fg->dev,
            "P450 FG current_ua=%d avg_ua=%d aux_stat=%#x system_status=%#x rc_now=%d rc_avg=%d source=SM5714_RANGE_25_10 signed=1 range_x25=%d aux_bit3=%d status_flag=UNVERIFIED\n",
            now,avg,aux,stat,now_rc,avg_rc,aux<0?-1:!!(aux&0x400),aux<0?-1:!!(aux&0x8));
        dev_info(fg->dev,
            "P451 BATTERY current_mA=%d avg_mA=%d status=Unknown source=FG_NO_SEC_BATTERY\n",
            now/1000,avg/1000);
    }
    if (first || fg->a52g_guard_cycle++ % 15 == 0) {
		dev_info(fg->dev,
			"P447G CHG_GUARD SAMPLE source=%s batt_therm_deciC=%d fg_temp_deciC=%d effective_deciC=%d threshold=%d VBUS=%d status1=%d latched=%d\n",
			source,ntc,fgtemp,temp,A52G_GUARD_TEMP_DECI_C,vbus,st,
			fg->a52g_inhibit_latched);
		dev_info(fg->dev,"P447H TEMP therm=%d chg=%d fg=%d src=%s vbus=%d interval_s=30\n",
			 ntc,a52h_chg_ntc_temp(fg),fgtemp,source,vbus);
	}
	if (!unsafe && !fg->a52g_inhibit_latched) {
        if (first || fg->a52g_guard_cycle % 15 == 1)
            a52p450_chg_state(fg,"MONITOR");
        return 0;
    }
	if (vbus==0 && !fg->a52g_inhibit_latched)
		return 0;

	/* Confirm current register, mask only ENQ4FET, read it back.
	 * IMPORTANT: NO writes besides CHG_CNTL1(0x13) BIT(3)->0.
	 */
	ctrl=i2c_smbus_read_byte_data(fg->a52g_chg,A52G_CHG_CNTL1);
	if (ctrl<0) {
		dev_err_ratelimited(fg->dev,
			"P447G CHG_GUARD INHIBIT_FAILED step=read_cntl1 rc=%d VBUS=%d fg_temp=%d\n",
			ctrl,vbus,temp);
		return ctrl;
	}
	if (ctrl & A52G_CHG_ENQ4FET) {
		rc=i2c_smbus_write_byte_data(fg->a52g_chg,A52G_CHG_CNTL1,
				ctrl & ~A52G_CHG_ENQ4FET);
		if (rc<0) {
			dev_err_ratelimited(fg->dev,
				"P447G CHG_GUARD INHIBIT_FAILED step=write_cntl1 rc=%d VBUS=%d fg_temp=%d\n",
				rc,vbus,temp);
			return rc;
		}
	}
	readback=i2c_smbus_read_byte_data(fg->a52g_chg,A52G_CHG_CNTL1);
	if (readback<0 || (readback & A52G_CHG_ENQ4FET)) {
		dev_err_ratelimited(fg->dev,
			"P447G CHG_GUARD INHIBIT_FAILED step=verify rc=%d cntl1_before=%#x readback=%d\n",
			readback<0 ? readback : -EIO,ctrl,readback);
		return readback<0 ? readback : -EIO;
	}
	if (first || (ctrl&A52G_CHG_ENQ4FET) ||
        fg->a52g_guard_cycle % 15 == 0)
        a52p450_chg_state(fg,"AFTER_INHIBIT");
    if (!fg->a52g_inhibit_latched || (ctrl & A52G_CHG_ENQ4FET))
		dev_warn(fg->dev,
			"P447G CHG_GUARD INHIBIT_OK source=%s batt_therm_deciC=%d fg_temp_deciC=%d effective_deciC=%d VBUS=%d deviceid=%#x cntl1_before=%#x after=%#x mask=0x08 never_reenable=1\n",
			source,ntc,fgtemp,temp,vbus,die,ctrl,readback);
	fg->a52g_inhibit_latched=true;
	return 0;
}



/* A52_PHASE452_H0_MANUAL_SAFE_CHARGER_V1 */
/* P460B_H0_GUARD_2S_WDT_10S_V1: original P449 guard cadence retained. */
#define A52H0_CHGCNTL2 0x18
#define A52H0_WDTCNTL 0x22
#define A52H0_BATREG 0x1a
#define A52H0_VBUSCNTL 0x15
#define A52H01_MAX_BOOTLOADER_VBUSCNTL 0x44
/* A52_PHASE452_H01_VBUS44_VERIFY_IO_V1 */
#define A52H0_FACTORY1 0x25
#define A52H0_CHG_FAST_500MA 0x20
#define A52H0_FLOAT_4380_CODE 0x27
#define A52H0_90S_WDT 0x04
#define A52H0_MAX_TICKS (14400UL*HZ)
#define A52H0_KICK_MIN_MS 10000
#define A52H0_OVERVOLT_FAULT_MV 4430
static int a52h0_regread(struct a52d_fg *fg,u8 r)
{
    return i2c_smbus_read_byte_data(fg->a52g_chg,r);
}
/* This is the ONLY H0 register-write callsite. Audit its allowlist. */
static int a52h0_write_masked(struct a52d_fg *fg,u8 reg,u8 mask,u8 val)
{
    int old,rc,after;
    if ((reg!=A52H0_CHGCNTL2 && reg!=A52H0_WDTCNTL &&
         reg!=A52G_CHG_CNTL1 && reg!=A52H0_VBUSCNTL) ||
        (reg==A52H0_VBUSCNTL && mask!=0x7f) ||
        (reg==A52H0_CHGCNTL2 && mask!=0xff) ||
        (reg==A52G_CHG_CNTL1 && mask!=A52G_CHG_ENQ4FET) ||
        (reg==A52H0_WDTCNTL && (mask&~0x4f)))
        return -EPERM;
    if(reg==A52H0_CHGCNTL2 && val>0x86)return -EPERM;
#if IS_ENABLED(CONFIG_P479_H4)
    if(reg==A52H0_VBUSCNTL &&
       (!fg->p479_h4_auto || val>0x44))return -EPERM;
#else
    if(reg==A52H0_VBUSCNTL)return -EPERM;
#endif
    old=a52h0_regread(fg,reg);
    if(old<0)return old;
    if(reg==A52H0_VBUSCNTL && ((old&~mask)|(val&mask))>0x44)
        return -EPERM;
    rc=i2c_smbus_write_byte_data(fg->a52g_chg,reg,(old&~mask)|(val&mask));
    if(rc<0)return rc;
    after=a52h0_regread(fg,reg);
    if(after<0)return after;
    /* WDT kick/reset bits are self-clearing: do not require them to
     * remain set after the write. All other bits must match.
     */
    if(reg==A52H0_WDTCNTL)mask &= ~(BIT(3)|BIT(6));
    if((after&mask)!=(val&mask))return -EIO;
    return 0;
}
static void a52h0_state(struct a52d_fg *fg, const char *to,
                        const char *reason)
{
    const char *from=fg->a52h0_state ? fg->a52h0_state : "OFF";
    if (strcmp(from,to)) {
        dev_info(fg->dev,"P452 H0 STATE from=%s to=%s reason=%s\n",
                 from,to,reason);
        fg->a52h0_state=to;
    }
}

#if IS_ENABLED(CONFIG_P476_H2)
static int p476_h2_off(struct a52d_fg *fg, const char *reason);
#endif

#if IS_ENABLED(CONFIG_P479_H4)
static int p479_h4_restore(struct a52d_fg *fg,const char *why);
#endif
static void a52h0_off(struct a52d_fg *fg,const char *why,bool permanent)
{
    int ctrl=a52h0_regread(fg,A52G_CHG_CNTL1);
    int rc=0;
#if IS_ENABLED(CONFIG_P479_H4)
    if(fg->p479_h4_auto && p479_h4_restore(fg,why))
        permanent=true;
#endif
#if IS_ENABLED(CONFIG_P476_H2)
    /* Return to 500 mA BEFORE any existing H0 disable/stop path. */
    if((fg->p476_h2_fast || fg->p476_h2_enabled) &&
       p476_h2_off(fg,why))
        permanent=true;
#endif
    if(ctrl<0 || (ctrl&A52G_CHG_ENQ4FET))
        rc=a52h0_write_masked(fg,A52G_CHG_CNTL1,A52G_CHG_ENQ4FET,0);
    if(!rc) {
        int after=a52h0_regread(fg,A52G_CHG_CNTL1);
        if(after>=0 && !(after&A52G_CHG_ENQ4FET))
            rc=a52h0_write_masked(fg,A52H0_WDTCNTL,BIT(0),0);
        else rc=-EIO;
    }
    fg->a52h0_active=false;
    /* Release on every stop path, including FAULT and failed enable. */
    if(fg->a52h0_wake_held) {
        __pm_relax(fg->a52h0_ws);
        fg->a52h0_wake_held=false;
    }
    if(fg->a52h0_usb && !IS_ERR(fg->a52h0_usb))
        power_supply_changed(fg->a52h0_usb);
    if(permanent || rc)fg->a52h0_killed=true;
    if(fg->a52h0_killed) {
        fg->a52h0_armed=false;
        a52h0_state(fg,"FAULT_LATCHED",why);
    } else if(!strcmp(why,"TOP_OFF")) {
        a52h0_state(fg,"FULL",why);
    } else if(!strcmp(why,"BOOT_PRE_ARM")) {
        a52h0_state(fg,"HANDOVER",why);
    } else if(!strcmp(why,"MANUAL_DISARM")) {
        a52h0_state(fg,"OFF",why);
    } else {
        fg->a52h0_armed=false;
        fg->a52h0_rearm=true;
        a52h0_state(fg,"STOPPED",why);
    }
    dev_info(fg->dev,"P452 H0 STOP reason=%s\n",why);
    dev_warn(fg->dev,
       "P452 H0 OFF reason=%s rc=%d permanent=%d killed=%d\n",
       why,rc,permanent,fg->a52h0_killed);
    if(fg->psy)power_supply_changed(fg->psy);
}
static bool a52h0_sensor_safe(struct a52d_fg *fg,int *b,int *c)
{
    *b=a52g_ntc_temp(fg);
    *c=a52h_chg_ntc_temp(fg);
    return (*b>=150 && *b<=410 && *c>=-200 && *c<500);
}
static int a52h0_verify(struct a52d_fg *fg,bool for_enable,
                        int *vb,int *vcell,int *st2,int *ntc,int *chg_ntc)
{
    int st,fc,flt,ctrl,input,die,rc=0;
    *vcell=a52d_mv(fg);
    st=a52h0_regread(fg,A52G_CHG_STATUS1);
    *st2=a52h0_regread(fg,A52P450_CHG_STATUS2);
    fc=a52h0_regread(fg,A52H0_FACTORY1);
    flt=a52h0_regread(fg,A52H0_BATREG);
    ctrl=a52h0_regread(fg,A52G_CHG_CNTL1);
    input=a52h0_regread(fg,A52H0_VBUSCNTL);
    die=a52h0_regread(fg,A52G_CHG_DEVICEID);
    /* A failed I2C read is transient. Hardware violations remain faults. */
    if(st<0 || *st2<0 || fc<0 || flt<0 ||
       ctrl<0 || input<0 || die<0 || *vcell<0) {
        rc=-EAGAIN;
        goto fail;
    }
    if(*vcell<3000 || *vcell>A52H0_OVERVOLT_FAULT_MV) {
        rc=-EIO;
        goto fail;
    }
    /* Read-only bootloader input ceiling, with H0 charge current 500 mA. */
    if(die!=fg->a52h0_id || (fc&(BIT(0)|BIT(1))) ||
       (flt&0x3f)!=A52H0_FLOAT_4380_CODE ||
       input>A52H01_MAX_BOOTLOADER_VBUSCNTL) {
        rc=-EUCLEAN;
        goto fail;
    }
    *vb=!!(st&BIT(0));
    if(*st2&BIT(7)) { rc=-ETIME; goto fail; }
    if(*vcell>4380) { rc=-EOVERFLOW; goto fail; }
    if(!a52h0_sensor_safe(fg,ntc,chg_ntc)) {
        rc=-ERANGE;
        goto fail;
    }
    if(!*vb) { rc=-ENOLINK; goto fail; }
    if(for_enable && (ctrl&A52G_CHG_ENQ4FET)) {
        rc=-EALREADY;
        goto fail;
    }
    return 0;
fail:
    dev_warn(fg->dev,
        "P452 H0.1 VERIFY_FAIL rc=%d st1=%d st2=%d factory1=%d batreg=%d vbuscntl=%d id=%d cntl1=%d vcell=%d\n",
        rc,st,*st2,fc,flt,input,die,ctrl,*vcell);
    return rc;
}

/* A52_P475_S2_CABLE_READ_ONLY_V1
 * TG sm5714-private.h:19,38: MUIC 0x4a (8-bit) = 0x25 (7-bit),
 * DEVICETYPE1 reg 0x07. TG sm5714-muic.h:68-81: BC1.2 bits.
 * Read-only SMBus xfer on existing charger adapter: no fake device.
 */
#if IS_ENABLED(CONFIG_P475_S2_CABLE)
static int p475_s2_dt1(struct a52d_fg *fg)
{
	union i2c_smbus_data data;
	int rc;

	if (!fg->a52g_chg || !fg->a52g_chg->adapter)
		return -ENODEV;
	rc = i2c_smbus_xfer(fg->a52g_chg->adapter, 0x25, 0,
			    I2C_SMBUS_READ, 0x07, I2C_SMBUS_BYTE_DATA, &data);
	if (rc < 0)
		return rc;
	return data.byte;
}
static const char *p475_s2_type(struct a52d_fg *fg, int *dt1)
{
	int status = a52h0_regread(fg, A52G_CHG_STATUS1);
	int type;
	if (status < 0) {
		*dt1 = status;
		return "UNKNOWN";
	}
	if (!(status & BIT(0))) {
		*dt1 = -ENOLINK;
		return "NONE";
	}
	type = p475_s2_dt1(fg);
	*dt1 = type;
	if (type < 0)
		return "UNKNOWN";
	/* Contradictory classifications fail closed instead of guessing. */
	if ((type & BIT(5)) && (type & BIT(6)))
		return "UNKNOWN";
	if (type & BIT(5))
		return (type & (BIT(1)|BIT(3))) ? "UNKNOWN" : "AFC";
	if (type & BIT(6))
		return (type & (BIT(1)|BIT(3))) ? "UNKNOWN" : "QC";
	if ((type & BIT(2)) &&
	    !(type & (BIT(1)|BIT(3)|BIT(4)|BIT(7))))
		return "DCP";
	if ((type & BIT(3)) && !(type & (BIT(1)|BIT(2))))
		return "CDP";
	if ((type & BIT(1)) && !(type & (BIT(2)|BIT(3))))
		return "SDP";
	return "UNKNOWN";
}
static void p475_s2_log(struct a52d_fg *fg)
{
	int dt1;
	const char *type = p475_s2_type(fg, &dt1);
	if (dt1 == fg->p475_s2_last_dt1 &&
	    !strcmp(type, fg->p475_s2_last_type))
		return;
	dev_info(fg->dev, "P475 S2 CABLE type=%s dt1=%d vbuscntl=read_only\n",
		 type, dt1);
	fg->p475_s2_last_dt1 = dt1;
	fg->p475_s2_last_type = type;
}
static ssize_t a52_cable_type_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct a52d_fg *fg = dev_get_drvdata(dev);
	const char *type;
	int dt1;
	if (!fg)
		return -ENODEV;
	mutex_lock(&fg->a52h0_lock);
	type = p475_s2_type(fg, &dt1);
	mutex_unlock(&fg->a52h0_lock);
	return scnprintf(buf, PAGE_SIZE, "%s dt1=%d\n", type, dt1);
}
static DEVICE_ATTR_RO(a52_cable_type);
static void p475_s2_remove(void *arg)
{
	struct a52d_fg *fg = arg;
	device_remove_file(fg->dev, &dev_attr_a52_cable_type);
}
static void p475_s2_setup(struct a52d_fg *fg)
{
	int rc = device_create_file(fg->dev, &dev_attr_a52_cable_type);
	if (!rc) {
		rc = devm_add_action_or_reset(fg->dev, p475_s2_remove, fg);
		if (!rc)
			p475_s2_log(fg);
	}
	if (rc)
		dev_warn(fg->dev, "P475 S2 CABLE type=UNKNOWN sysfs_rc=%d\n", rc);
}
#else
static const char *p475_s2_type(struct a52d_fg *fg, int *dt1)
{
	*dt1 = -ENODEV;
	return "UNKNOWN";
}
static void p475_s2_log(struct a52d_fg *fg) {}
static void p475_s2_setup(struct a52d_fg *fg) {}
#endif


/* A52_P476_H2_WALL_FAST_CURRENT_MANUAL_V1
 * Independent Kconfig default=n and sysfs runtime default=0.
 * TG sm5714_charger.c:243-264: 1000mA -> CHGCNTL2 code 0x40.
 * H0's 0x44 input ceiling is PRESERVED. Do not write VBUSCNTL.
 */
#if IS_ENABLED(CONFIG_P476_H2)
#define P476_H2_1000MA_CODE 0x40
#define P476_H2_BATTERY_STOP_DECI_C 390
#define P476_H2_CHARGER_STOP_DECI_C 450
static const char *p475_s2_type(struct a52d_fg *fg, int *dt1);

/* Locking: a52h0_lock is held by all callers, including H0 OFF paths. */
static int p476_h2_off(struct a52d_fg *fg, const char *reason)
{
	int charge_reg, rc=0;
	bool was_on=fg->p476_h2_fast || fg->p476_h2_enabled;

	/* Absolutely no extra I2C reads/writes on the H0-only default. */
	if (!was_on)
		return 0;
	charge_reg=a52h0_regread(fg,A52H0_CHGCNTL2);
	if (fg->p476_h2_fast || charge_reg==P476_H2_1000MA_CODE ||
	    (IS_ENABLED(CONFIG_P477_H3) && was_on)) {
		rc=a52h0_write_masked(fg,A52H0_CHGCNTL2,0xff,
				      A52H0_CHG_FAST_500MA);
		if (rc) {
			/* If 500mA cannot be restored, caller must inhibit H0. */
			fg->a52h0_killed=true;
			fg->a52h0_armed=false;
		}
	}
	fg->p476_h2_fast=false;
	fg->p476_h2_enabled=false;
#if IS_ENABLED(CONFIG_P477_H3)
    fg->p477_h3_actual_ma=500;
#endif
	if (was_on || rc)
		dev_warn(fg->dev,"P476 H2 OFF reason=%s rc=%d cur=500_request\n",
			 reason,rc);
	return rc;
}

/* Shared with legacy H2: a single real H2 ON format string. */
static void p476_h2_on_log(struct a52d_fg *fg,int vbus,const char *type)
{
    dev_info(fg->dev,"P476 H2 ON cur=1000 vbuscntl=0x%02x type=%s\n",
             vbus,type);
}
/* P477_H3_SINGLE_WRITER_STEPPED_DCP_ONLY_V1
 * TG sm5714_charger.c:243-264 maps FCC to CHGCNTL2 0x18.
 * The live A52 TA DT specifies input 1800 / battery FCC 2100 mA.
 * H0 remains the only writer. Do not alter VBUSCNTL/BATREG/FACTORY1.
 */
#if IS_ENABLED(CONFIG_P477_H3)
#define P477_H3_CODE_500 0x20
#define P477_H3_CODE_1000 0x40
#define P477_H3_CODE_1400 0x59
#define P477_H3_CODE_1500 0x60
#define P477_H3_CODE_2100 0x86
#define P477_H3_STEP_INTERVAL (60 * HZ)
static int p477_h3_code(int ma)
{
    switch (ma) {
    case 500: return P477_H3_CODE_500;
    case 1000: return P477_H3_CODE_1000;
    case 1400: return P477_H3_CODE_1400;
    case 1500: return P477_H3_CODE_1500;
    case 2100: return P477_H3_CODE_2100;
    default: return -EINVAL;
    }
}
static void p477_h3_reset_stable(struct a52d_fg *fg, int bat, int chg)
{
    fg->p477_h3_stable_since = jiffies;
    fg->p477_h3_bat_anchor = bat;
    fg->p477_h3_chg_anchor = chg;
}
static void p477_h3_limit(struct a52d_fg *fg, int ma, const char *reason)
{
    dev_warn(fg->dev,"P477 H3 LIMIT ma=%d reason=%s\n",ma,reason);
}
/* Always entered with the H0 mutex, after H0's original guard pass. */
static void p477_h3_tick(struct a52d_fg *fg, int bat, int chg)
{
    int vbus, fc, fl, st1, ctrl, wdt, cur, dt1, rc, limit;
    int actual = fg->p477_h3_actual_ma;
    int target = fg->p477_h3_target_ma;
    int next;
    const char *type, *reason = NULL;
    unsigned long now = jiffies;

    if (!fg->p476_h2_enabled && !fg->p476_h2_fast)
        return;

    if (!fg->a52h0_active || !fg->a52h0_armed ||
        fg->a52h0_killed || fg->a52g_inhibit_latched ||
        !fg->a52h0_state ||
        strcmp(fg->a52h0_state,"CHARGING")) {
        reason = "H0_NOT_CHARGING";
        goto off;
    }
    /* Both independent physical NTCs must be valid; fail to 500 on doubt.
     * Existing H0 stronger 41C/50C shutdown stays in effect.
     */
    if (bat < 150 || bat > 900 || chg < -200 || chg > 900) {
        reason = "NTC_INVALID";
        goto off;
    }
    if (bat >= 410)
        fg->p477_h3_bat_hot = true;
    if (fg->p477_h3_bat_hot && bat <= 390)
        fg->p477_h3_bat_hot = false;
    if (fg->p477_h3_bat_hot) {
        reason = "BAT_41C_HYSTERESIS_39C";
        goto off;
    }
    if (chg >= 550) {
        reason = "CHARGER_55C";
        goto off;
    }

    vbus = a52h0_regread(fg,A52H0_VBUSCNTL);
    fc = a52h0_regread(fg,A52H0_FACTORY1);
    fl = a52h0_regread(fg,A52H0_BATREG);
    st1 = a52h0_regread(fg,A52G_CHG_STATUS1);
    ctrl = a52h0_regread(fg,A52G_CHG_CNTL1);
    wdt = a52h0_regread(fg,A52H0_WDTCNTL);
    cur = a52h0_regread(fg,A52H0_CHGCNTL2);
    if (vbus<0 || fc<0 || fl<0 || st1<0 || ctrl<0 ||
        wdt<0 || cur<0) {
        reason = "READ_ERROR";
        goto off;
    }
    if (vbus != 0x44 || vbus > A52H01_MAX_BOOTLOADER_VBUSCNTL) {
        reason = "VBUS_NOT_0X44";
        goto off;
    }
    if ((fc & (BIT(0)|BIT(1))) ||
        (fl & 0x3f) != A52H0_FLOAT_4380_CODE ||
        !(st1 & BIT(0)) || !(ctrl & A52G_CHG_ENQ4FET) ||
        (wdt & 0x07) != (A52H0_90S_WDT|BIT(0))) {
        reason = "FACTORY_FLOAT_WDT_OR_ENABLE";
        goto off;
    }
    type = p475_s2_type(fg,&dt1);
    if (strcmp(type,"DCP")) {
        reason = "NOT_DCP";
        goto off;
    }
    if (cur != p477_h3_code(actual)) {
        reason = "CHGCNTL2_DRIFT";
        goto off;
    }

    /* Clamp on charger NTC without changing the accepted user values. */
    limit = chg >= 480 ? 1400 : 2100;
    if (target > limit) {
        p477_h3_limit(fg,limit,"CHARGER_48C_CAP");
        target=limit;
    }
    if (target < actual) {
        /* Immediate demotion, including directly to 500 on a hot NTC. */
        next = target;
    } else if (target == actual) {
        return;
    } else {
        /* Refuse escalation until a full 60 s at stable temperatures.
         * >1C drift in either NTC restarts the stability window.
         */
        if (abs(bat-fg->p477_h3_bat_anchor)>10 ||
            abs(chg-fg->p477_h3_chg_anchor)>10) {
            p477_h3_reset_stable(fg,bat,chg);
            p477_h3_limit(fg,actual,"TEMPERATURE_MOVING");
            return;
        }
        if (time_before(now,fg->p477_h3_last_step+P477_H3_STEP_INTERVAL) ||
            time_before(now,fg->p477_h3_stable_since+P477_H3_STEP_INTERVAL))
            return;
        /* Even when user requests 2100 directly, climb one step only. */
        if (actual==500) next=1000;
        else if (actual==1000) next=1500;
        else if (actual==1400) next=1500;
        else if (actual==1500) next=2100;
        else {
            reason="UNEXPECTED_ACTUAL_CURRENT";
            goto off;
        }
        if (next>target)next=target;
        if (next>limit)next=limit;
    }
    if (next>2100 || next<500 || p477_h3_code(next)<0) {
        reason="INVALID_CURRENT";
        goto off;
    }
    rc=a52h0_write_masked(fg,A52H0_CHGCNTL2,0xff,
                          (u8)p477_h3_code(next));
    if (rc) {
        reason="CURRENT_WRITE_OR_READBACK";
        goto off;
    }
    fg->p477_h3_actual_ma=next;
    fg->p476_h2_fast=(next>500);
    fg->p477_h3_last_step=now;
    p477_h3_reset_stable(fg,bat,chg);
    dev_info(fg->dev,
      "P477 H3 SET ma=%d code=0x%02x vbuscntl=0x44 type=DCP bat=%d chg=%d\n",
      next,p477_h3_code(next),bat,chg);
    if(next==1000)
        p476_h2_on_log(fg,vbus,type);
    return;
off:
    p477_h3_limit(fg,500,reason?reason:"UNKNOWN");
    rc=p476_h2_off(fg,reason?reason:"P477_H3_SAFETY");
    if(rc) {
        dev_err(fg->dev,"P477 H3 RESTORE_500_FAILED rc=%d inhibit=1\n",rc);
        /* Existing H0 tick sees killed flag and executes OFF. */
        fg->a52h0_killed=true;
        fg->a52h0_armed=false;
    }
    fg->p477_h3_actual_ma=500;
}
static ssize_t a52_h2_ma_show(struct device *dev,
       struct device_attribute *attr,char *buf)
{
    struct a52d_fg *fg=dev_get_drvdata(dev);
    return scnprintf(buf,PAGE_SIZE,"%d\n",fg->p477_h3_target_ma);
}
static ssize_t a52_h2_ma_store(struct device *dev,
       struct device_attribute *attr,const char *buf,size_t len)
{
    struct a52d_fg *fg=dev_get_drvdata(dev);
    int ma;
    if(kstrtoint(buf,10,&ma))return -EINVAL;
    if(ma!=500 && ma!=1000 && ma!=1500 && ma!=2100)return -EINVAL;
    mutex_lock(&fg->a52h0_lock);
    /* Only a request; no direct register write in sysfs context. */
    fg->p477_h3_target_ma=ma;
    mutex_unlock(&fg->a52h0_lock);
    return len;
}
static DEVICE_ATTR(a52_h2_ma,0600,a52_h2_ma_show,a52_h2_ma_store);
static void p477_h3_remove(void *arg)
{
    struct a52d_fg *fg=arg;
    device_remove_file(fg->dev,&dev_attr_a52_h2_ma);
}
static void p477_h3_setup(struct a52d_fg *fg)
{
    int rc;
    fg->p477_h3_target_ma=1000;
    fg->p477_h3_actual_ma=500;
    fg->p477_h3_last_step=jiffies;
    p477_h3_reset_stable(fg,a52g_ntc_temp(fg),a52h_chg_ntc_temp(fg));
    rc=device_create_file(fg->dev,&dev_attr_a52_h2_ma);
    if(!rc)
        rc=devm_add_action_or_reset(fg->dev,p477_h3_remove,fg);
    if(rc)dev_err(fg->dev,"P477 H3 LIMIT ma=500 reason=SYSFS_ERROR rc=%d\n",rc);
}
#endif /* CONFIG_P477_H3 */


/* P479_MGBATT_SHADOW3_COMPARE_NO_WRITES_V1 */
/* P479_H4_AUTO_5V_SOLE_WRITER_V1
 * S2 MUIC plus real VBUS; no QC/AFC/PD negotiation, no shadow writes.
 * All H4 changes use a52h0_write_masked() with its allowlist/ceilings.
 * Existing H0/P449 41C/50C hard stops remain stronger than TG's 55C cap.
 */
#if IS_ENABLED(CONFIG_P479_H4)
static int p479_h4_icl_code(int ma)
{
    if (ma == 475 || ma == 500 || ma == 1000 ||
        ma == 1500 || ma == 1800)
        return (ma - 100) / 25;
    return -EINVAL;
}
static int p479_h4_fcc_code(int ma)
{
    int ua = ma * 1000;
    if (ma != 500 && ma != 550 && ma != 1000 &&
        ma != 1400 && ma != 1500 && ma != 2100)
        return -EINVAL;
    return 7 + (ua - 109375) / 15625;
}
static int p479_h4_one_write(struct a52d_fg *fg, bool input, int ma)
{
    int code = input ? p479_h4_icl_code(ma) : p479_h4_fcc_code(ma);
    int reg = input ? A52H0_VBUSCNTL : A52H0_CHGCNTL2;
    int mask = input ? 0x7f : 0xff;
    int rc, rb;
    if (code < 0)
        return -EINVAL;
    rc = a52h0_write_masked(fg, reg, mask, (u8)code);
    rb = a52h0_regread(fg, reg);
    if (!rc && rb < 0)
        rc = rb;
    if (!rc && (rb & mask) != code)
        rc = -EIO;
    if (input)
        dev_info_ratelimited(fg->dev,
          "P479 H4 ICL_SET ma=%d code=0x%02x readback=0x%02x rc=%d\n",
          ma, code, rb & 0xff, rc);
    else
        dev_info_ratelimited(fg->dev,
          "P479 H4 FCC_SET ma=%d code=0x%02x readback=0x%02x rc=%d\n",
          ma, code, rb & 0xff, rc);
    return rc;
}
/* Cross-direction transitions are handled safely: first lower FCC,
 * then lower ICL, then raise ICL, then raise FCC.
 */
static int p479_h4_apply(struct a52d_fg *fg, int icl, int fcc)
{
    int old_icl = a52h0_regread(fg,A52H0_VBUSCNTL);
    int old_fcc = a52h0_regread(fg,A52H0_CHGCNTL2);
    int in = p479_h4_icl_code(icl), ch = p479_h4_fcc_code(fcc);
    int rc;
    if (old_icl < 0 || old_fcc < 0 || in < 0 || ch < 0 ||
        old_icl > 0x44 || old_fcc > 0x86)
        return -EUCLEAN;
    old_icl &= 0x7f;
    if (ch < old_fcc) {
        rc = p479_h4_one_write(fg,false,fcc);
        if (rc) return rc;
    }
    if (in != old_icl) {
        rc = p479_h4_one_write(fg,true,icl);
        if (rc) return rc;
    }
    if (ch > old_fcc) {
        rc = p479_h4_one_write(fg,false,fcc);
        if (rc) return rc;
    }
    fg->p479_h4_applied_icl = icl;
    fg->p479_h4_applied_fcc = fcc;
    return 0;
}
/* Must never call a52h0_off from here; H0 owns the shutdown path. */
static int p479_h4_restore(struct a52d_fg *fg, const char *why)
{
    int rc1,rc2,old_fcc,old_icl;
    if (!fg->p479_h4_auto)
        return 0;
    old_fcc = a52h0_regread(fg,A52H0_CHGCNTL2);
    old_icl = a52h0_regread(fg,A52H0_VBUSCNTL);
    if (old_fcc == 0x20 && old_icl == 0x10) {
        fg->p479_h4_applied_fcc = 500;
        fg->p479_h4_applied_icl = 500;
        return 0;
    }
    /* Force FCC first on unplug, regardless of register read errors. */
    rc1 = p479_h4_one_write(fg,false,500);
    rc2 = p479_h4_one_write(fg,true,500);
    if (rc1 || rc2) {
        dev_err_ratelimited(fg->dev,
            "P479 H4 FAULT reason=RESTORE_500_FAILED\n");
        fg->a52h0_killed = true;
        fg->a52h0_armed = false;
        return rc1 ? rc1 : rc2;
    }
    fg->p479_h4_applied_icl = 500;
    fg->p479_h4_applied_fcc = 500;
    dev_info_ratelimited(fg->dev,"P479 H4 LIMIT ma=500 reason=%s\n",why);
    return 0;
}
static void p479_h4_fault(struct a52d_fg *fg, const char *reason)
{
    dev_err_ratelimited(fg->dev,"P479 H4 FAULT reason=%s\n",reason);
    if (p479_h4_restore(fg,"FAULT_RECOVERY")) {
        fg->a52h0_killed = true;
        fg->a52h0_armed = false;
    }
    /* No silent retry to higher current after a write/readback fault. */
    fg->p479_h4_auto = false;
    fg->p479_h4_faulted = true;
}
static void p479_h4_pre_tick(struct a52d_fg *fg)
{
    const char *type, *reason = "CABLE_TABLE";
    int dt1,bt,ct;
    unsigned long now = jiffies;
    if (!fg->p479_h4_auto)
        return;
    type = p475_s2_type(fg,&dt1);
    if (!fg->p479_h4_pending || strcmp(type,fg->p479_h4_pending)) {
        fg->p479_h4_pending = type;
        fg->p479_h4_ready = false;
        fg->p479_h4_seen = now;
        /* A changed/untrusted cable immediately loses any old high vote. */
        if (p479_h4_restore(fg,"CABLE_CHANGE")) {
            p479_h4_fault(fg,"CABLE_CHANGE_RESTORE");
            return;
        }
    }
    if (!fg->p479_h4_ready &&
        time_after_eq(now,fg->p479_h4_seen+HZ)) {
        fg->p479_h4_ready=true;
        fg->p479_h4_type=type;
        fg->p477_h3_actual_ma=500;
        fg->p477_h3_last_step=now;
        p477_h3_reset_stable(fg,a52g_ntc_temp(fg),a52h_chg_ntc_temp(fg));
        dev_info_ratelimited(fg->dev,
           "P479 H4 CABLE type=%s icl=%d fcc_target=%d reason=%s\n",
           type,!strcmp(type,"DCP")?1800:!strcmp(type,"CDP")?1500:
                 !strcmp(type,"SDP")?475:500,
           !strcmp(type,"DCP")?2100:!strcmp(type,"CDP")?1500:
                 !strcmp(type,"SDP")?550:500,reason);
    }
    if (!fg->p479_h4_ready)
        return;
    if (!strcmp(type,"NONE")) {
        /* H0's original worker stops charging on VBUS loss.
         * A fully sampled unplug permits one rearm on a later insert.
         */
        fg->p479_h4_saw_unplug=true;
        fg->a52h0_armed=false;
        return;
    }
    if (fg->p479_h4_saw_unplug) {
        fg->p479_h4_rearm_allowed=true;
        fg->p479_h4_saw_unplug=false;
    }
    /* Never arm from a stale/failed MUIC read without real VBUS. */
    {
        int st=a52h0_regread(fg,A52G_CHG_STATUS1);
        if (st<0 || !(st&BIT(0)))
            return;
    }
    /* The permanent H0 kill and the original P449 thermal latch prevail. */
    if (fg->a52h0_killed || fg->a52h0_active || fg->a52h0_armed ||
        !fg->p479_h4_rearm_allowed || fg->a52h0_verify_io_streak)
        return;
    bt=a52g_ntc_temp(fg);
    ct=a52h_chg_ntc_temp(fg);
    if (!fg->a52h0_ntc_valid ||
        !time_after_eq(now,fg->a52h0_ntc_since+10*HZ) ||
        bt<150 || bt>410 || ct< -200 || ct>=500)
        return;
    if ((fg->a52h0_rearm || fg->a52g_inhibit_latched) &&
        (bt<170 || bt>390 || ct>450))
        return;
    fg->a52g_inhibit_latched=false;
    fg->a52h0_rearm=false;
    fg->a52h0_armed=true;
    fg->p479_h4_rearm_allowed=false;
    a52h0_state(fg,"ARMED","P479_H4_CABLE_REARM");
}
/* Invoked after H0 verification and only inside its 2s guard worker. */
static noinline void p479_h4_tick(struct a52d_fg *fg, int bat, int chg)
{
    int icl=500,fcc=500,actual,target,step,rc;
    const char *type = fg->p479_h4_type;
    const char *why = "CABLE_TABLE";
    unsigned long now=jiffies;
    if (!fg->p479_h4_ready || !type)
        return;
    if (!fg->a52h0_active || !fg->a52h0_armed ||
        fg->a52h0_killed || fg->a52g_inhibit_latched ||
        !fg->a52h0_state || strcmp(fg->a52h0_state,"CHARGING"))
        return;
    if (!strcmp(type,"DCP")) { icl=1800; fcc=2100; }
    else if (!strcmp(type,"CDP")) { icl=1500; fcc=1500; }
    else if (!strcmp(type,"SDP")) { icl=475; fcc=550; }
    /* Unknown, PDIC, AFC, QC remain 500/500, no negotiation. */
    if (bat<150 || bat>900 || chg< -200 || chg>900) {
        icl=500; fcc=500; why="NTC_INVALID";
    } else {
        if(bat>=410) fg->p477_h3_bat_hot=true;
        if(fg->p477_h3_bat_hot && bat<=390)
            fg->p477_h3_bat_hot=false;
        if (fg->p477_h3_bat_hot) {
            icl=500; fcc=500; why="BAT_41C_HOLD_39C_RECOVER";
        } else if (chg>=550) {
            icl=500; fcc=500; why="CHARGER_55C";
        } else if (chg>=480) {
            icl=min(icl,1000); fcc=min(fcc,1400);
            why="CHARGER_48C_CAP";
        }
    }
    actual = fg->p477_h3_actual_ma;
    target = fcc;
    if (!strcmp(type,"DCP") && target>actual) {
        if (abs(bat-fg->p477_h3_bat_anchor)>10 ||
            abs(chg-fg->p477_h3_chg_anchor)>10) {
            p477_h3_reset_stable(fg,bat,chg);
            target=actual; why="TEMPERATURE_MOVING";
        } else if (time_before(now,fg->p477_h3_last_step+P477_H3_STEP_INTERVAL) ||
                   time_before(now,fg->p477_h3_stable_since+P477_H3_STEP_INTERVAL)) {
            target=actual; why="H3_STABLE_DWELL";
        } else {
            if(actual==500) step=1000;
            else if(actual==1000) step=1500;
            else if(actual==1400) step=1500;
            else if(actual==1500) step=2100;
            else { p479_h4_fault(fg,"H3_STEPPER_INVALID"); return; }
            target=min(target,step);
        }
    }
    if (target<500 || target>2100 || icl>1800) {
        p479_h4_fault(fg,"REQUEST_ABOVE_CEILING");
        return;
    }
    rc=p479_h4_apply(fg,icl,target);
    if (rc) {
        p479_h4_fault(fg,"SET_OR_READBACK");
        return;
    }
    mgbatt_shadow_compare(fg->dev,type,bat,chg,
                          a52d_soc(fg),a52d_mv(fg),
                          fg->p479_h4_applied_icl,
                          fg->p479_h4_applied_fcc,
                          fg->a52h0_full);
    if (target!=actual) {
        fg->p477_h3_actual_ma=target;
        fg->p477_h3_last_step=now;
        p477_h3_reset_stable(fg,bat,chg);
    }
    if (strcmp(why,"CABLE_TABLE"))
        dev_info_ratelimited(fg->dev,"P479 H4 LIMIT ma=%d reason=%s\n",
                             min(icl,target),why);
}
static ssize_t a52_h4_auto_show(struct device *dev,
             struct device_attribute *attr,char *buf)
{
    struct a52d_fg *fg=dev_get_drvdata(dev);
    return scnprintf(buf,PAGE_SIZE,"%d\n",fg->p479_h4_auto);
}
static ssize_t a52_h4_auto_store(struct device *dev,
             struct device_attribute *attr,const char *buf,size_t len)
{
    struct a52d_fg *fg=dev_get_drvdata(dev);
    bool enable;
    int rc=0;
    if (!fg || kstrtobool(buf,&enable))
        return -EINVAL;
    mutex_lock(&fg->a52h0_lock);
    if (enable) {
        if (fg->a52h0_killed) {
            rc=-EPERM;
        } else if (!fg->p479_h4_auto) {
            if (fg->p476_h2_fast || fg->p476_h2_enabled)
                rc=p476_h2_off(fg,"H4_TAKES_CONTROL");
            if (!rc) {
                fg->p479_h4_auto=true;
                fg->p479_h4_rearm_allowed=true;
                fg->p479_h4_saw_unplug=false;
                fg->p479_h4_faulted=false;
                fg->p479_h4_pending=NULL;
                fg->p479_h4_type=NULL;
                fg->p479_h4_ready=false;
            }
        }
    } else if (fg->p479_h4_auto) {
        rc=p479_h4_restore(fg,"AUTO_DISARM");
        fg->p479_h4_auto=false;
        if (!fg->a52h0_active)
            fg->a52h0_armed=false;
        if (rc)
            a52h0_off(fg,"P479_H4_RESTORE_FAILED",true);
    }
    mutex_unlock(&fg->a52h0_lock);
    return rc?rc:len;
}
static DEVICE_ATTR(a52_h4_auto,0600,a52_h4_auto_show,a52_h4_auto_store);
static void p479_h4_remove(void *arg)
{
    struct a52d_fg *fg=arg;
    device_remove_file(fg->dev,&dev_attr_a52_h4_auto);
}
static void p479_h4_setup(struct a52d_fg *fg)
{
    int rc;
    fg->p479_h4_auto=false; /* P479 runtime default OFF */
    fg->p479_h4_applied_icl=500;
    fg->p479_h4_applied_fcc=500;
    rc=device_create_file(fg->dev,&dev_attr_a52_h4_auto);
    if (!rc) rc=devm_add_action_or_reset(fg->dev,p479_h4_remove,fg);
    if (rc) dev_err(fg->dev,"P479 H4 FAULT reason=SYSFS_UNAVAILABLE\n");
}
#else
static void p479_h4_pre_tick(struct a52d_fg *fg) {}
static void p479_h4_tick(struct a52d_fg *fg,int bat,int chg) {}
static void p479_h4_setup(struct a52d_fg *fg) {}
#endif

static void p476_h2_tick(struct a52d_fg *fg,int bat,int chg)
{
	int vbus,fl,fc,cur,dt1,ctrl,wdt,st1,rc;
	const char *type, *reason=NULL;

#if IS_ENABLED(CONFIG_P479_H4)
    if (fg->p479_h4_auto) {
        p479_h4_tick(fg,bat,chg);
        return;
    }
#endif
#if IS_ENABLED(CONFIG_P477_H3)
    p477_h3_tick(fg,bat,chg);
    return;
#endif
	/* A 2-second H0 guard pass and H0 CHARGING status are prerequisites. */
	if (!fg->p476_h2_enabled && !fg->p476_h2_fast)
		return;
	if (!fg->a52h0_active || !fg->a52h0_armed ||
	    fg->a52h0_killed || fg->a52g_inhibit_latched ||
	    strcmp(fg->a52h0_state,"CHARGING")) {
		reason="H0_NOT_CHARGING";
		goto off;
	}
	if (bat<150 || bat>=P476_H2_BATTERY_STOP_DECI_C) {
		reason="BATTERY_39C";
		goto off;
	}
	if (chg< -200 || chg>=P476_H2_CHARGER_STOP_DECI_C) {
		reason="CHARGER_45C";
		goto off;
	}
	vbus=a52h0_regread(fg,A52H0_VBUSCNTL);
	fc=a52h0_regread(fg,A52H0_FACTORY1);
	fl=a52h0_regread(fg,A52H0_BATREG);
	st1=a52h0_regread(fg,A52G_CHG_STATUS1);
	ctrl=a52h0_regread(fg,A52G_CHG_CNTL1);
	wdt=a52h0_regread(fg,A52H0_WDTCNTL);
	cur=a52h0_regread(fg,A52H0_CHGCNTL2);
	if (vbus<0 || fc<0 || fl<0 || st1<0 || ctrl<0 ||
	    wdt<0 || cur<0) {
		reason="READ_ERROR";
		goto off;
	}
	/* H0.1 rejects >0x44, so BOTH bounds are kept. */
	if (vbus < 0x44 || vbus>A52H01_MAX_BOOTLOADER_VBUSCNTL) {
		reason="INPUT_NOT_WALL";
		goto off;
	}
	if ((fc&(BIT(0)|BIT(1))) ||
	    (fl&0x3f)!=A52H0_FLOAT_4380_CODE) {
		reason="FACTORY_OR_FLOAT";
		goto off;
	}
	if (!(st1&BIT(0)) || !(ctrl&A52G_CHG_ENQ4FET) ||
	    (wdt&0x07)!=(A52H0_90S_WDT|BIT(0))) {
		reason="VBUS_OR_WDT";
		goto off;
	}
	/*
	 * Both tracks are independently gated. When S2 is unavailable
	 * or the real MUIC disagrees, NO elevated current is permitted.
	 */
	type=p475_s2_type(fg,&dt1);
	if (strcmp(type,"DCP")) {
		dev_warn(fg->dev,
			 "P476 H2 CABLE_DISAGREE vbuscntl=0x%02x type=%s dt1=%d\n",
			 vbus,type,dt1);
		reason="CABLE_NOT_DCP";
		goto off;
	}
	if (fg->p476_h2_fast) {
		if (cur!=P476_H2_1000MA_CODE) {
			reason="CHGCNTL2_DRIFT";
			goto off;
		}
		return;
	}
	/* Never increase from an unknown fast-charge register value. */
	if (cur!=A52H0_CHG_FAST_500MA) {
		reason="PRE_RAISE_CHGCNTL2_DRIFT";
		goto off;
	}
	rc=a52h0_write_masked(fg,A52H0_CHGCNTL2,0xff,
			      P476_H2_1000MA_CODE);
	if (rc) {
		reason="1000MA_WRITE_OR_READBACK";
		goto off;
	}
	fg->p476_h2_fast=true;
	p476_h2_on_log(fg,vbus,type);
	return;
off:
	rc=p476_h2_off(fg,reason ? reason : "UNKNOWN");
	if (rc)
		dev_err(fg->dev,"P476 H2 OFF reason=500MA_RESTORE_FAILED rc=%d; H0 must inhibit\n",rc);
}
static ssize_t a52_h2_enable_show(struct device *dev,
				 struct device_attribute *attr,char *buf)
{
	struct a52d_fg *fg=dev_get_drvdata(dev);
	if (!fg)
		return -ENODEV;
	return scnprintf(buf,PAGE_SIZE,"%d\n",fg->p476_h2_enabled);
}
static ssize_t a52_h2_enable_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf,size_t len)
{
	struct a52d_fg *fg=dev_get_drvdata(dev);
	bool enable;
	int rc=0;
	if (!fg || kstrtobool(buf,&enable))
		return -EINVAL;
	mutex_lock(&fg->a52h0_lock);
#if IS_ENABLED(CONFIG_P479_H4)
	if (fg->p479_h4_auto) {
		mutex_unlock(&fg->a52h0_lock);
		return -EBUSY;
	}
#endif
	if (enable) {
		/* Manual arm is refused unless H0 is already charging. */
		if (!fg->a52h0_active || !fg->a52h0_armed ||
		    fg->a52h0_killed || fg->a52g_inhibit_latched ||
		    strcmp(fg->a52h0_state,"CHARGING")) {
            dev_warn(fg->dev,
             "P476 H2 ARM_REFUSED state=%s active=%d armed=%d killed=%d latch=%d\n",
             fg->a52h0_state?fg->a52h0_state:"OFF",fg->a52h0_active,
             fg->a52h0_armed,fg->a52h0_killed,fg->a52g_inhibit_latched);
			rc=-EPERM;
        }
		else {
			fg->p476_h2_enabled=true;
#if IS_ENABLED(CONFIG_P477_H3)
            fg->p477_h3_last_step=jiffies;
            p477_h3_reset_stable(fg,a52g_ntc_temp(fg),
                                 a52h_chg_ntc_temp(fg));
#endif
        }
	} else {
		rc=p476_h2_off(fg,"MANUAL_DISARM");
		if (rc)
			a52h0_off(fg,"H2_RESTORE_FAILED",true);
	}
	mutex_unlock(&fg->a52h0_lock);
	return rc ? rc : len;
}
static DEVICE_ATTR(a52_h2_enable,0600,a52_h2_enable_show,a52_h2_enable_store);
static void p476_h2_remove(void *arg)
{
	struct a52d_fg *fg=arg;
	device_remove_file(fg->dev,&dev_attr_a52_h2_enable);
}
static void p476_h2_setup(struct a52d_fg *fg)
{
	int rc;
	fg->p476_h2_enabled=false;
	fg->p476_h2_fast=false;
	rc=device_create_file(fg->dev,&dev_attr_a52_h2_enable);
	if (!rc)
		rc=devm_add_action_or_reset(fg->dev,p476_h2_remove,fg);
	if (rc)
		dev_warn(fg->dev,"P476 H2 OFF reason=SYSFS_UNAVAILABLE rc=%d\n",rc);
#if IS_ENABLED(CONFIG_P477_H3)
    if(!rc)p477_h3_setup(fg);
#endif
}
#else
static void p476_h2_setup(struct a52d_fg *fg) {}
#endif

/* P477_MGBATT_SHADOW_DT_READ_ONLY_V1 */
static void a52h0_tick(struct a52d_fg *fg)
{
    int vb=0,mv=0,st2=-1,tbat=-10000,tchg=-10000,rc,wdt,ctrl;
    unsigned long now=jiffies;
    mutex_lock(&fg->a52h0_lock);
    p475_s2_log(fg);
#if IS_ENABLED(CONFIG_MGBATT_SHADOW)
    {
        int dt1;
        const char *type=p475_s2_type(fg,&dt1);
        mgbatt_shadow_tick(fg->dev,type,dt1,
           a52g_ntc_temp(fg),a52h_chg_ntc_temp(fg));
    }
#endif
    p479_h4_pre_tick(fg);
    /* Track ten consecutive seconds of both real physical NTCs valid. */
    {
        int bt=a52g_ntc_temp(fg),ct=a52h_chg_ntc_temp(fg);
        if(bt>=-200 && bt<=900 && ct>=-200 && ct<=900) {
            if(!fg->a52h0_ntc_valid) {
                fg->a52h0_ntc_valid=true;
                fg->a52h0_ntc_since=now;
            }
        } else {
            fg->a52h0_ntc_valid=false;
        }
    }
    /* Session timer resets only when unplugged, including while OFF. */
    {
        int st=a52h0_regread(fg,A52G_CHG_STATUS1);
        if(st>=0 && !(st&BIT(0))) {
            fg->a52h0_timer_started=false;
            fg->a52h0_full=false;
        }
    }
    if(fg->a52h0_killed || !fg->a52h0_armed) {
        if(fg->a52h0_active) a52h0_off(fg,"MANUAL_DISARM",false);
        /*
         * The first failed verify has already DISARMED and inhibited.
         * Observe at most two further guard samples in read-only mode.
         * Never auto re-arm after an I2C recovery.
         */
        if(!fg->a52h0_killed && fg->a52h0_verify_io_streak) {
            int check_rc=a52h0_verify(fg,false,&vb,&mv,&st2,
                                      &tbat,&tchg);
            if(check_rc==-EAGAIN) {
                fg->a52h0_verify_io_streak++;
                dev_warn(fg->dev,
                    "P452 H0.1 VERIFY_IO streak=%u active=0\n",
                    fg->a52h0_verify_io_streak);
                if(fg->a52h0_verify_io_streak>=3)
                    a52h0_off(fg,"VERIFY_IO_3",true);
            } else {
                dev_info(fg->dev,
                    "P452 H0.1 VERIFY_IO recovered rc=%d manual_rearm=1\n",
                    check_rc);
                fg->a52h0_verify_io_streak=0;
            }
        }
        goto done;
    }
    if(fg->a52g_inhibit_latched) {
        a52h0_off(fg,"guard",false);
        goto done;
    }
    rc=a52h0_verify(fg,!fg->a52h0_active,&vb,&mv,&st2,&tbat,&tchg);
    if(rc==-EAGAIN) {
        fg->a52h0_verify_io_streak++;
        dev_warn(fg->dev,
            "P452 H0.1 VERIFY_IO streak=%u active=%d\n",
            fg->a52h0_verify_io_streak,fg->a52h0_active);
        a52h0_off(fg,"VERIFY_IO",
                  fg->a52h0_verify_io_streak>=3);
        goto done;
    }
    /* Consecutive error counter resets on any valid I2C register set. */
    fg->a52h0_verify_io_streak=0;
    if(rc) {
        if(rc==-ETIME) {
            a52h0_off(fg,"WDT_EXPIRED",true);
            if(a52h0_write_masked(fg,A52H0_WDTCNTL,BIT(6),BIT(6)))
                dev_err(fg->dev,"P452 H0 WDT expired clear FAILED\n");
        } else if(rc==-EOVERFLOW) {
            fg->a52h0_full=true;
            a52h0_off(fg,"TOP_OFF",false);
        } else if(rc==-EALREADY) {
            a52h0_off(fg,"BOOT_PRE_ARM",false);
            fg->a52h0_armed=false;
            fg->a52h0_rearm=true;
        } else if(rc==-ERANGE || rc==-ENOLINK)
            a52h0_off(fg,rc==-ERANGE ? "temp" : "vbus",false);
        else
            a52h0_off(fg,"FAULT_VERIFY",true);
        goto done;
    }
    if(fg->a52h0_active) {
        if(time_after_eq(now,fg->a52h0_started+A52H0_MAX_TICKS)) {
            a52h0_off(fg,"timer",false);goto done;
        }
        if(st2&BIT(5)) {
            fg->a52h0_full=true;
            a52h0_off(fg,"TOP_OFF",false);goto done;
        }
        ctrl=a52h0_regread(fg,A52G_CHG_CNTL1);
        wdt=a52h0_regread(fg,A52H0_WDTCNTL);
        if(ctrl<0 || wdt<0 || !(ctrl&A52G_CHG_ENQ4FET) ||
           (wdt&0x07)!=(A52H0_90S_WDT|BIT(0))) {
            a52h0_off(fg,"WDT_OR_ENQ4FET_DRIFT",true);goto done;
        }
#if IS_ENABLED(CONFIG_P476_H2)
        p476_h2_tick(fg,tbat,tchg);
        if(fg->a52h0_killed) {
            a52h0_off(fg,"H2_RESTORE_FAILED",true);
            goto done;
        }
#endif
        /* Guard checks run every 2s; kick hardware WDT no faster than 10s. */
        if(time_before(now,fg->a52h0_last_kick+
                 msecs_to_jiffies(A52H0_KICK_MIN_MS)))goto done;
        rc=a52h0_write_masked(fg,A52H0_WDTCNTL,BIT(3),BIT(3));
        if(rc){a52h0_off(fg,"WDT_KICK_FAILED",true);goto done;}
        fg->a52h0_last_kick=now;
        dev_info(fg->dev,
           "P452 H0 KICK wdt=90s guard=2s min_kick=10s rc=0 battery=%d chg=%d mv=%d\n",
           tbat,tchg,mv);
        goto done;
    }
    if(fg->a52h0_timer_started &&
       time_after_eq(now,fg->a52h0_started+A52H0_MAX_TICKS)) {
        a52h0_off(fg,"timer",false);goto done;
    }
    if(fg->a52h0_full && mv>4310)goto done;
    if(fg->a52h0_full && mv<=4310) {
        fg->a52h0_full=false;
        dev_info(fg->dev,"P452 H0 RECHARGE vbat=%d\n",mv);
    }
    /* Confirm all limits before setting ENQ4FET. WDT first, always. */
    rc=a52h0_write_masked(fg,A52H0_CHGCNTL2,0xff,A52H0_CHG_FAST_500MA);
    if(rc)goto fail;
    rc=a52h0_write_masked(fg,A52H0_WDTCNTL,0x06,A52H0_90S_WDT);
    if(rc)goto fail;
    rc=a52h0_write_masked(fg,A52H0_WDTCNTL,BIT(0),BIT(0));
    if(rc)goto fail;
    rc=a52h0_write_masked(fg,A52H0_WDTCNTL,BIT(3),BIT(3));
    if(rc)goto fail;
    wdt=a52h0_regread(fg,A52H0_WDTCNTL);
    if(wdt<0 || (wdt&0x7)!=(A52H0_90S_WDT|BIT(0))) {
        rc=-EIO;goto fail;
    }
    if(!fg->a52h0_ws){rc=-ENODEV;goto fail;}
    /* Acquire BEFORE ENQ4FET to close the suspend window. */
    __pm_stay_awake(fg->a52h0_ws);
    fg->a52h0_wake_held=true;
    rc=a52h0_write_masked(fg,A52G_CHG_CNTL1,A52G_CHG_ENQ4FET,
                           A52G_CHG_ENQ4FET);
    if(rc)goto fail; /* a52h0_off releases wake on all errors */
    fg->a52h0_active=true;
    if(!fg->a52h0_timer_started) {
        fg->a52h0_started=now;
        fg->a52h0_timer_started=true;
    }
    fg->a52h0_last_kick=now;
    a52h0_state(fg,"CHARGING","enable");
    dev_info(fg->dev,
       "P452 H0 ENABLE cur=500 wdt=90 float=4380 batt=%d chg=%d\n",
       tbat,tchg);
    dev_warn(fg->dev,
       "P452 H0 ON manual=1 WDT=90s kick=before_enable current=500mA float=4380mV battery=%d chg=%d mv=%d readback=PASS\n",
       tbat,tchg,mv);
    if(fg->psy)power_supply_changed(fg->psy);
    if(fg->a52h0_usb && !IS_ERR(fg->a52h0_usb))
        power_supply_changed(fg->a52h0_usb);
    goto done;
fail:
    a52h0_off(fg,"ENABLE_FAILED",true);
done:
    mutex_unlock(&fg->a52h0_lock);
}
static ssize_t h0_arm_show(struct device *dev,
                           struct device_attribute *attr,char *buf)
{
    struct a52d_fg *fg=dev_get_drvdata(dev);
    return scnprintf(buf,PAGE_SIZE,"armed=%d active=%d killed=%d full=%d\n",
                     fg->a52h0_armed,fg->a52h0_active,
                     fg->a52h0_killed,fg->a52h0_full);
}
static ssize_t h0_arm_store(struct device *dev,
                            struct device_attribute *attr,
                            const char *buf,size_t len)
{
    struct a52d_fg *fg=dev_get_drvdata(dev);
    bool arm;
    if(kstrtobool(buf,&arm))return -EINVAL;
    mutex_lock(&fg->a52h0_lock);
#if IS_ENABLED(CONFIG_P477_H0_REARM_FIX)
    if(arm && fg->a52h0_active) {
        mutex_unlock(&fg->a52h0_lock);
        return len; /* idempotent: preserve CHARGING and latch */
    }
#endif
    if(arm) {
        int bt=a52g_ntc_temp(fg),ct=a52h_chg_ntc_temp(fg);
        if(fg->a52h0_killed) {
            mutex_unlock(&fg->a52h0_lock);
            return -EPERM;
        }
        if(!fg->a52h0_ntc_valid ||
           !time_after_eq(jiffies,fg->a52h0_ntc_since+10*HZ)) {
            mutex_unlock(&fg->a52h0_lock);
            return -EAGAIN;
        }
        if(bt<150 || bt>410 || ct< -200 || ct>=500 ||
           ((fg->a52h0_rearm || fg->a52g_inhibit_latched) &&
            (bt<170 || bt>390 || ct>450))) {
            mutex_unlock(&fg->a52h0_lock);
            return -ERANGE;
        }
        /* Only explicit manual re-arm clears P449's historic latch
         * after both physical NTCs recover; next worker runs guard first.
         */
        fg->a52g_inhibit_latched=false;
        fg->a52h0_rearm=false;
        a52h0_state(fg,"ARMED","manual_rearm");
    }
    fg->a52h0_armed=arm;
    if(!arm)a52h0_off(fg,"MANUAL_DISARM",false);
    mutex_unlock(&fg->a52h0_lock);
    dev_warn(fg->dev,"P452 H0 ARM optin=%d killed=%d\n",
             arm,fg->a52h0_killed);
    return len;
}
static ssize_t h0_kill_store(struct device *dev,
                             struct device_attribute *attr,
                             const char *buf,size_t len)
{
    struct a52d_fg *fg=dev_get_drvdata(dev);
    bool kill;
    if(kstrtobool(buf,&kill) || !kill)return -EINVAL;
    mutex_lock(&fg->a52h0_lock);
    fg->a52h0_killed=true;
    fg->a52h0_armed=false;
    a52h0_off(fg,"KILL_SWITCH",true);
    mutex_unlock(&fg->a52h0_lock);
    return len;
}
static DEVICE_ATTR(a52_h0_enable,0600,h0_arm_show,h0_arm_store);
static DEVICE_ATTR(h0_kill,0200,NULL,h0_kill_store);
static void a52h0_attr_cleanup(void *arg)
{
    struct a52d_fg *fg=arg;
    device_remove_file(fg->dev,&dev_attr_h0_kill);
    device_remove_file(fg->dev,&dev_attr_a52_h0_enable);
}
static int a52h0_attrs_create(struct a52d_fg *fg)
{
    int rc=device_create_file(fg->dev,&dev_attr_a52_h0_enable);
    if(rc)return rc;
    rc=device_create_file(fg->dev,&dev_attr_h0_kill);
    if(rc){device_remove_file(fg->dev,&dev_attr_a52_h0_enable);return rc;}
    rc=devm_add_action_or_reset(fg->dev,a52h0_attr_cleanup,fg);
    return rc;
}
/* Minimal ONLINE mirrors TG VBUS_POK. Without MUIC we do not classify cable. */
static enum power_supply_property a52h0_input_props[] = {
    POWER_SUPPLY_PROP_ONLINE,
};
static int a52h0_input_get(struct power_supply *psy,
                           enum power_supply_property prop,
                           union power_supply_propval *val)
{
    struct a52d_fg *fg=power_supply_get_drvdata(psy);
    int st;
    if(prop!=POWER_SUPPLY_PROP_ONLINE)return -EINVAL;
    st=a52h0_regread(fg,A52G_CHG_STATUS1);
    if(st<0)return st;
    val->intval=!!(st&BIT(0));
    return 0;
}
static const struct power_supply_desc a52h0_usb_desc={
    .name="usb", .type=POWER_SUPPLY_TYPE_USB,
    .properties=a52h0_input_props,
    .num_properties=ARRAY_SIZE(a52h0_input_props),
    .get_property=a52h0_input_get,
};
static int a52h0_register_inputs(struct a52d_fg *fg)
{
    struct power_supply_config cfg={ .drv_data=fg };
    fg->a52h0_usb=devm_power_supply_register(fg->dev,&a52h0_usb_desc,&cfg);
    if(IS_ERR(fg->a52h0_usb)) {
        int rc=PTR_ERR(fg->a52h0_usb);
        fg->a52h0_usb=NULL;
        return rc;
    }
    return 0;
}
static int a52h0_psy_status(struct a52d_fg *fg,bool online)
{
    if(fg->a52h0_full)return POWER_SUPPLY_STATUS_FULL;
    if(fg->a52h0_active)return POWER_SUPPLY_STATUS_CHARGING;
    return online?POWER_SUPPLY_STATUS_NOT_CHARGING:
                  POWER_SUPPLY_STATUS_DISCHARGING;
}


/* P463 CHG_GUARD PM_SYNC. PREPARE runs before device suspend.
 * The charging H0 wake source already prevents sleep while actively
 * charging; the PM notifier does not change that invariant.
 */
static int a52g_pm_notify(struct notifier_block *nb, unsigned long event,
                          void *unused)
{
    struct a52d_fg *fg = container_of(nb, struct a52d_fg, a52g_pm_nb);

    switch (event) {
    case PM_SUSPEND_PREPARE:
        WRITE_ONCE(fg->a52g_pm_paused, true);
        cancel_delayed_work_sync(&fg->a52g_guard_work);
        dev_info(fg->dev, "P463 CHG_GUARD SUSPEND_PREPARE cancelled=1 active=%d wake_held=%d\n",
                 fg->a52h0_active, fg->a52h0_wake_held);
        return NOTIFY_OK;
    case PM_POST_SUSPEND:
        fg->a52g_pm_grace_until = jiffies + msecs_to_jiffies(2000);
        WRITE_ONCE(fg->a52g_pm_paused, false);
        mod_delayed_work(system_freezable_wq, &fg->a52g_guard_work, 0);
        dev_info(fg->dev, "P463 CHG_GUARD POST_SUSPEND immediate_sample=1 VBUS=check_after_resume\n");
        return NOTIFY_OK;
    default:
        return NOTIFY_DONE;
    }
}
static void a52g_poll(struct work_struct *work)
{
    struct a52d_fg *fg = container_of(to_delayed_work(work),
                                     struct a52d_fg, a52g_guard_work);
    int err;
    unsigned long delay = msecs_to_jiffies(A52G_GUARD_POLL_MS);

    if (READ_ONCE(fg->a52g_pm_paused))
        return;
    err = a52g_sample(fg, false);
    if (READ_ONCE(fg->a52g_pm_paused)) {
        dev_warn_ratelimited(fg->dev,
            "P463 CHG_GUARD SUSPEND_INFLIGHT rc=%d no_h0_fault_latch\n", err);
        return;
    }
    if (err < 0) {
        /* If OFF during resume, defer just the transient bus error.
         * NEVER run H0 tick/enable without a successful guard sample.
         * If charging is active, preserve the existing fail-closed
         * emergency inhibit (a52h0_off), not a silent continuation.
         */
        if (!fg->a52h0_active &&
            time_before(jiffies, fg->a52g_pm_grace_until)) {
            dev_warn_ratelimited(fg->dev,
                "P463 CHG_GUARD POST_RESUME_IO rc=%d deferred=1 killed=0\n", err);
            delay = msecs_to_jiffies(200);
        } else {
            mutex_lock(&fg->a52h0_lock);
            a52h0_off(fg, "GUARD_IO", true);
            mutex_unlock(&fg->a52h0_lock);
        }
    } else {
        a52h0_tick(fg);
    }
    if (!READ_ONCE(fg->a52g_pm_paused))
        mod_delayed_work(system_freezable_wq, &fg->a52g_guard_work, delay);
}


static int a52h0_regread(struct a52d_fg *fg,u8 r);
static int a52h0_psy_status(struct a52d_fg *fg,bool online);
static enum power_supply_property a52d_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
    POWER_SUPPLY_PROP_CURRENT_NOW,
    POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_STATUS,
    POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_TECHNOLOGY,
};

static int a52d_get_property(struct power_supply *psy,
			    enum power_supply_property p,
			    union power_supply_propval *v)
{
	struct a52d_fg *fg=power_supply_get_drvdata(psy);
	int rc;
	switch (p) {
	case POWER_SUPPLY_PROP_PRESENT:
		rc=a52d_soc(fg);
		if (rc<0)return rc;
		v->intval=1;return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		rc=a52d_soc(fg);
		if (rc<0)return rc;
		v->intval=rc;return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
        return a52p450_read_current_ua(fg,A52P450_FG_CURR,&v->intval,NULL,NULL);
    case POWER_SUPPLY_PROP_CURRENT_AVG:
        return a52p450_read_current_ua(fg,A52P450_FG_CURR_AVG,&v->intval,NULL,NULL);
    case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		rc=a52d_mv(fg);
		if (rc<0)return rc;
		v->intval=rc*1000;return 0;
	case POWER_SUPPLY_PROP_TEMP:
		rc=a52d_temp(fg);
		if (rc<-400)return -EIO;
		v->intval=rc;return 0;
	case POWER_SUPPLY_PROP_ONLINE: {
        int st=a52h0_regread(fg,A52G_CHG_STATUS1);
        if(st<0)return st;
        v->intval=!!(st&BIT(0));
        return 0;
    }
    case POWER_SUPPLY_PROP_STATUS: {
        int st=a52h0_regread(fg,A52G_CHG_STATUS1);
        if(st<0)return st;
        v->intval=a52h0_psy_status(fg,!!(st&BIT(0)));
        return 0;
    }
    /* P452 H0 original status is intentionally suppressed. */
		/* Status is supplied above by audited H0 controller. */
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		v->intval=POWER_SUPPLY_TECHNOLOGY_LION;return 0;
	default:return -EINVAL;
	}
}
static const struct power_supply_desc a52e_fg_desc={
	.name="sm5714-fuelgauge",
	.type=POWER_SUPPLY_TYPE_UNKNOWN,
	.properties=a52d_props,
	.num_properties=ARRAY_SIZE(a52d_props),
	.get_property=a52d_get_property,
};

static void a52d_fg_snapshot(struct work_struct *work)
{
	struct a52d_fg *fg=container_of(to_delayed_work(work),
			struct a52d_fg,snapshot);
	int soc=a52d_soc(fg),mv=a52d_mv(fg),temp=a52d_temp(fg);
	/* Never fake a percentage if the gauge cannot be read. */
	if (soc>=0 && mv>=0 && temp>=-400)
		dev_info(fg->dev,
			"P447 W2 battery 30S soc=%d mv=%d temp=%d status=UNKNOWN cable=UNAVAILABLE rc=0\n",
			soc,mv,temp);
	else
		dev_err(fg->dev,
			"P447 W2 battery 30S invalid soc=%d mv=%d temp=%d rc=%d\n",
			soc,mv,temp,-EIO);
}
static void a52d_fg_unregister(void *arg)
{
	struct a52d_fg *fg=arg;
	if (fg->a52g_pm_registered) {
        unregister_pm_notifier(&fg->a52g_pm_nb);
        fg->a52g_pm_registered = false;
    }
    cancel_delayed_work_sync(&fg->a52g_guard_work);
    /* The psy devres may already be gone here: do NOT call
     * a52h0_off() because it notifies fg->psy. Disable directly.
     */
    if(fg->a52h0_active) {
        int rc=a52h0_write_masked(fg,A52G_CHG_CNTL1,
                                   A52G_CHG_ENQ4FET,0);
        if(!rc)rc=a52h0_write_masked(fg,A52H0_WDTCNTL,BIT(0),0);
        if(rc)dev_err(fg->dev,"P452 H0 REMOVE inhibit FAILED rc=%d\n",rc);
        fg->a52h0_active=false;
        fg->a52h0_killed=true;
        fg->a52h0_armed=false;
    }
    if(fg->a52h0_wake_held) {
        __pm_relax(fg->a52h0_ws);
        fg->a52h0_wake_held=false;
    }
    if(fg->a52h0_ws) {
        wakeup_source_unregister(fg->a52h0_ws);
        fg->a52h0_ws=NULL;
    }
	cancel_delayed_work_sync(&fg->snapshot);
	i2c_unregister_device(fg->fg);
}

/* A52_PHASE447F_SM5714_FG_CHILD_V1
 * Mimic the exact TG MFD child device path:
 * .../i2c-34/34-0049/sm5714-fuelgauge/power_supply/sm5714-fuelgauge.
 * Other SM5714 children are NOT instantiated until their actual hardware
 * control drivers and Samsung policy are safely integrated.
 */
static void a52f_fg_child_unregister(void *arg)
{
	platform_device_unregister(arg);
}
static void a52f_fg_psy_unregister(void *arg)
{
	power_supply_unregister(arg);
}

static int a52d_fg_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct a52d_fg *fg;
	struct power_supply_config psy_cfg={};
	int rc,dev_id,soc,mv,temp;

	rc=a52_447_probe_allowed(&client->dev,A52_447_SM5714);
	if (rc)return rc;
	if (!i2c_check_functionality(client->adapter,I2C_FUNC_SMBUS_WORD_DATA))
		return dev_err_probe(&client->dev,-EOPNOTSUPP,
				     "P447 W2 FG SMBus word not supported\n");
	fg=devm_kzalloc(&client->dev,sizeof(*fg),GFP_KERNEL);
	if (!fg)return -ENOMEM;
	mutex_init(&fg->lock);
	mutex_init(&fg->a52g_ntc_lock);
    mutex_init(&fg->a52h0_lock);
    fg->p475_s2_last_dt1=-999;
    fg->p475_s2_last_type="INIT";
	/* TG parse_dt() reads the sm5714-fuelgauge DT node. Missing
	 * policy is NOT a reason to invent 920; use pinned A52 R02's
	 * capacity_max=1000, min=0 only for read-only comparison.
	 */
	fg->a52f_capacity_max=1000;
	fg->a52f_capacity_min=0;
	fg->a52f_calc_type=0x1f;
	{
		struct device_node *np=of_find_node_by_name(NULL,
							"sm5714-fuelgauge");
		if (np) {
			of_property_read_u32(np,"fuelgauge,capacity_max",
					    &fg->a52f_capacity_max);
			of_property_read_u32(np,"fuelgauge,capacity_min",
					    &fg->a52f_capacity_min);
			of_property_read_u32(np,
				"fuelgauge,capacity_calculation_type",
				&fg->a52f_calc_type);
			of_node_put(np);
		}
	}
	if (fg->a52f_capacity_max<=fg->a52f_capacity_min ||
	    fg->a52f_capacity_max>1000) {
		dev_err(&client->dev,
			"P447F SOC invalid DT capacity_max=%u min=%u rc=%d\n",
			fg->a52f_capacity_max,fg->a52f_capacity_min,-EINVAL);
		return -EINVAL;
	}
	dev_info(&client->dev,
		"P447F SOC TG_DT max=%u min=%u type=%#x mode=compare_only (no inferred 920)\n",
		fg->a52f_capacity_max,fg->a52f_capacity_min,
		fg->a52f_calc_type);
	fg->dev=&client->dev;
    i2c_set_clientdata(client,fg);
	INIT_DELAYED_WORK(&fg->snapshot,a52d_fg_snapshot);
	INIT_DELAYED_WORK(&fg->a52g_guard_work,a52g_poll);
	fg->a52p450_started=jiffies;
    fg->a52p451_last_vbus=-2;
    fg->a52g_chg=client;
	/* MFD primary is 0x49, FG is a separate secondary address 0x71. */
	fg->fg=i2c_new_dummy_device(client->adapter,A52D_SM5714_FG_ADDR);
	if (IS_ERR(fg->fg))
		return dev_err_probe(fg->dev,PTR_ERR(fg->fg),
				     "P447 W2 FG secondary addr=0x71 unavailable\n");
	rc=devm_add_action_or_reset(&client->dev,a52d_fg_unregister,fg);
	if (rc)return rc;
	dev_id=i2c_smbus_read_word_data(fg->fg,A52D_FG_ID);
	if (dev_id<0 || (dev_id & 0xff00)) {
		dev_err(fg->dev,"P447 W2 sm5714 FG_PROBE bad_id=%d rc=%d\n",
			dev_id,dev_id<0?dev_id:-ENODEV);
		return dev_id<0?dev_id:-ENODEV;
	}
	soc=a52d_soc(fg);mv=a52d_mv(fg);temp=a52d_temp(fg);

	if (IS_ENABLED(CONFIG_A52_447G_CHG_GUARD)) {
		if (!i2c_check_functionality(client->adapter,
					I2C_FUNC_SMBUS_BYTE_DATA))
			return dev_err_probe(&client->dev,-EOPNOTSUPP,
				"P447G CHG_GUARD SMBus byte DATA required\n");
		/* Must run BEFORE exposing the temporary Android battery.
		 * This is best-effort software safety; OFF does not imply
		 * hardware charge circuitry is guaranteed safe.
		 */
		rc=a52g_sample(fg,true);
		if (rc<0)
			return dev_err_probe(&client->dev,rc,
				"P447G CHG_GUARD initial verification failed; no battery supplier registered\n");
	}	if (soc<0 || mv<0 || temp< -400) {
		dev_err(fg->dev,"P447 W2 sm5714 FG_PROBE invalid soc=%d mv=%d temp=%d rc=%d\n",
			soc,mv,temp,-EIO);
		return -ENODEV;
	}
	/* Publish the FG power_supply from its TG MFD child, not I2C root.
	 * The new child is a REAL kernel platform device, not a symlink.
	 */
	fg->a52f_fg_child=platform_device_alloc("sm5714-fuelgauge",
						  PLATFORM_DEVID_NONE);
	if (!fg->a52f_fg_child)
		return -ENOMEM;
	fg->a52f_fg_child->dev.parent=&client->dev;
	rc=platform_device_add(fg->a52f_fg_child);
	if (rc) {
		platform_device_put(fg->a52f_fg_child);
		return dev_err_probe(&client->dev,rc,
				    "P447F SM5714 FG_CHILD platform add failed\n");
	}
	rc=devm_add_action_or_reset(&client->dev,
				    a52f_fg_child_unregister,
				    fg->a52f_fg_child);
	if (rc)return rc;
	dev_info(&client->dev,
		 "P447F SM5714 FG_CHILD parent=%s child=%s bus=%d addr=%#x rc=0\n",
		 dev_name(&client->dev),dev_name(&fg->a52f_fg_child->dev),
		 client->adapter->nr,client->addr);
	psy_cfg.drv_data=fg;
	psy_cfg.of_node=client->dev.of_node;
	fg->psy=power_supply_register(&fg->a52f_fg_child->dev,&a52e_fg_desc,
					  &psy_cfg);
	if (IS_ERR(fg->psy))
		return dev_err_probe(fg->dev,PTR_ERR(fg->psy),
				     "P447 W2 battery POWER_SUPPLY register failed\n");
	/* The I2C parent releases psy FIRST, child SECOND, dummy FG LAST.
	 * No unowned or unbound devm power_supply resources survive removal.
	 */
	rc=devm_add_action_or_reset(&client->dev,
				    a52f_fg_psy_unregister,fg->psy);
	if (rc)return rc;
	dev_info(fg->dev,
		"P447 W2 sm5714 FG_PROBE rc=0 bus=%d addr=0x%x fg_addr=0x71 irq=NOT_USED fg_id=%#x read_only=1\n",
		client->adapter->nr,client->addr,dev_id);
	dev_info(fg->dev,
		"P447 W2 battery FIRST soc=%d mv=%d temp=%d status=UNKNOWN cable=UNAVAILABLE rc=0\n",
		soc,mv,temp);
	schedule_delayed_work(&fg->snapshot,msecs_to_jiffies(30000));
    /* Bootloader may leave ENQ4FET ON. Force it OFF before
     * exposing h0_arm, even if both NTCs are already valid.
     */
    fg->a52h0_state="OFF";
    fg->a52h0_rearm=true;
    fg->a52h0_id=a52h0_regread(fg,A52G_CHG_DEVICEID);
    if(fg->a52h0_id<0) {
        fg->a52h0_killed=true;
        dev_err(fg->dev,"P452 H0 INIT_FAIL chip id rc=%d; P449 guard retained\n",
                fg->a52h0_id);
    } else {
        /* Bootloader ENQ4FET set is HANDOVER, not an H0 failure. */
        a52h0_off(fg,"BOOT_PRE_ARM",false);
        if(fg->a52h0_killed)
            dev_err(fg->dev,"P458 H0 INIT_FAIL BOOT_PRE_ARM inhibit, keep P449 guard\\n");
    }
    if(!fg->a52h0_killed) {
        fg->a52h0_ws=wakeup_source_register(fg->dev,"a52-h0-charging");
        if(!fg->a52h0_ws) {
            fg->a52h0_killed=true;
            dev_err(fg->dev,"P452 H0 INIT_FAIL wakeup source; P449 guard retained\n");
        }
    }
    if(!fg->a52h0_killed) {
        rc=a52h0_attrs_create(fg);
        if(rc) {
            fg->a52h0_killed=true;
            dev_err(fg->dev,"P452 H0 INIT_FAIL sysfs rc=%d; P449 guard retained\n",rc);
        }
    }
    if(!fg->a52h0_killed) {
        rc=a52h0_register_inputs(fg);
        if(rc) {
            fg->a52h0_killed=true;
            dev_err(fg->dev,"P452 H0 INIT_FAIL usb rc=%d; P449 guard retained\n",rc);
        }
    }
    if(fg->a52h0_killed)
        dev_warn(fg->dev,"P452 H0 FAULT_LATCHED init; no arm, P449 guard scheduled\n");
    p475_s2_setup(fg);
    p476_h2_setup(fg);
    p479_h4_setup(fg);
    /* Fail probe closed if PM protection cannot be registered.
     * Devres FG cleanup unregisters first and drains pending work.
     */
    fg->a52g_pm_nb.notifier_call = a52g_pm_notify;
    {
        int pmrc = register_pm_notifier(&fg->a52g_pm_nb);
        if (pmrc) {
            dev_err(fg->dev,
                    "P463 CHG_GUARD PM notifier registration rc=%d\n", pmrc);
            return pmrc;
        }
        fg->a52g_pm_registered = true;
    }
	if (IS_ENABLED(CONFIG_A52_447G_CHG_GUARD))
		mod_delayed_work(system_freezable_wq, &fg->a52g_guard_work,
			msecs_to_jiffies(A52G_GUARD_POLL_MS));
	return 0;
}

/* A52_PHASE447E_SAMSUNG_BATTERY_PLATFORM_PATH_V1
 * This is a deliberately read-only bridge, NOT the full Samsung sec_battery
 * driver. Binding the actual samsung,sec-battery DT node places 'battery'
 * below samsung_mobile_device:battery, as TG's sec_battery does.
 * Future production sec_battery replaces this bridge entirely.
 */
struct a52e_battery {
	struct mutex a52h_cap_lock;
	int a52h_cap_max,a52h_cap_min,a52h_cap_nominal,a52h_cap_margin;
	int a52h_nominal_full_mah;
	bool a52h_written;
	struct platform_device *a52h_pdev;
	struct delayed_work a52h_ntc_retry;
	u32 a52h_attempts;
	struct power_supply *fg;
};
static int a52e_battery_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct a52e_battery *bat=power_supply_get_drvdata(psy);
	if (psp==POWER_SUPPLY_PROP_TEMP) {
		struct a52d_fg *fg=power_supply_get_drvdata(bat->fg);
		int ntc=a52g_ntc_temp(fg);
		if (ntc>=-200 && ntc<=900) {
			val->intval=ntc;
			return 0;
		}
		/* FG die remains labeled fallback, not claimed cell temperature. */
		return power_supply_get_property(bat->fg,psp,val);
	}
	if (psp==POWER_SUPPLY_PROP_CAPACITY) {
		struct a52d_fg *fg=power_supply_get_drvdata(bat->fg);
		int raw=a52f_soc_deci(fg);
		int scaled,max,min;
		bool written;
		if (raw<0)return raw;
		mutex_lock(&bat->a52h_cap_lock);
		max=bat->a52h_cap_max;
		min=bat->a52h_cap_min;
		written=bat->a52h_written;
		mutex_unlock(&bat->a52h_cap_lock);
		if (max<=min)return -ERANGE;
		/* TG 0.1% integer math, capped, then TRUNCATE to %.
		 * Do NOT change FG SRAM registers or charge controls.
		 */
		scaled=clamp_val((raw-min)*1000/(max-min),0,1000);
		val->intval=scaled/10;
		pr_info_ratelimited(
			"P447H SOC raw=%d capacity_max=%d capacity_min=%d scaled=%d ui=%d writer=%s no_NV_write=1\n",
			raw,max,min,scaled,val->intval,written?"sysfs":"default");
		return 0;
	}
	return power_supply_get_property(bat->fg,psp,val);
}

static const struct power_supply_desc a52e_battery_desc={
	.name="battery",
	.type=POWER_SUPPLY_TYPE_BATTERY,
	.properties=a52d_props,
	.num_properties=ARRAY_SIZE(a52d_props),
	.get_property=a52e_battery_get_property,
};

/* A52_PHASE447H_LEARNED_SOC_SYSFS_V1.
 * Source: TG sm5714_fg_get_scaled_capacity, sec_device_sysfs
 * BATT_CAPACITY_MAX / BATT_READ_RAW_SOC. No kernel writes to learned FG NV.
 */
static ssize_t batt_capacity_max_show(struct device *dev,
				      struct device_attribute *attr,char *buf)
{
	struct power_supply *psy=dev_get_drvdata(dev);
	struct a52e_battery *bat=power_supply_get_drvdata(psy);
	int max;
	mutex_lock(&bat->a52h_cap_lock);
	max=bat->a52h_cap_max;
	mutex_unlock(&bat->a52h_cap_lock);
	return scnprintf(buf,PAGE_SIZE,"%d\n",max);
}
static ssize_t batt_capacity_max_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf,size_t count)
{
	struct power_supply *psy=dev_get_drvdata(dev);
	struct a52e_battery *bat=power_supply_get_drvdata(psy);
	int value,old,lower;
	if (kstrtoint(buf,10,&value))
		return -EINVAL;
	mutex_lock(&bat->a52h_cap_lock);
	lower=max_t(int,bat->a52h_cap_min+1,
		    bat->a52h_cap_nominal-bat->a52h_cap_margin);
	if (value<lower || value>bat->a52h_cap_nominal) {
		mutex_unlock(&bat->a52h_cap_lock);
		return -ERANGE;
	}
	old=bat->a52h_cap_max;
	bat->a52h_cap_max=value;
	bat->a52h_written=true;
	mutex_unlock(&bat->a52h_cap_lock);
	power_supply_changed(psy); /* notify Android after learned-capacity restore */
	dev_info(dev,
		 "P447H SOC writer=sysfs pid=%d comm=%s cap_old=%d cap_new=%d range=%d..%d no_NV_write=1\n",
		 task_pid_nr(current),current->comm,old,value,lower,
		 bat->a52h_cap_nominal);
	return count;
}
static ssize_t batt_read_raw_soc_show(struct device *dev,
				      struct device_attribute *attr,char *buf)
{
	struct power_supply *psy=dev_get_drvdata(dev);
	struct a52e_battery *bat=power_supply_get_drvdata(psy);
	int deci=a52f_soc_deci(power_supply_get_drvdata(bat->fg));
	if (deci<0)return deci;
	/* TG RAW capacity type reports 0.01%, ten times raw-deci. */
	return scnprintf(buf,PAGE_SIZE,"%d\n",deci*10);
}
static ssize_t batt_full_capacity_show(struct device *dev,
				       struct device_attribute *attr,char *buf)
{
	struct power_supply *psy=dev_get_drvdata(dev);
	struct a52e_battery *bat=power_supply_get_drvdata(psy);
	/* A52 DT battery,battery_full_capacity, nominal units mAh;
	 * not a claimed live/learned CHARGE_COUNTER.
	 */
	return scnprintf(buf,PAGE_SIZE,"%d\n",bat->a52h_nominal_full_mah);
}
/* The original TG SYSFS_DEVICE_ATTR group uses mode 0664. This interim
 * port limits learned-capacity writes to owner/group (0660); the vendor
 * ueventd/init must still supply the expected system:radio ownership.
 */
static DEVICE_ATTR(batt_capacity_max,0660,
		   batt_capacity_max_show,batt_capacity_max_store);
static DEVICE_ATTR(batt_read_raw_soc,0444,batt_read_raw_soc_show,NULL);
static DEVICE_ATTR(batt_full_capacity,0444,batt_full_capacity_show,NULL);
static void a52h_attrs_release(void *arg)
{
	struct device *dev=arg;
	device_remove_file(dev,&dev_attr_batt_full_capacity);
	device_remove_file(dev,&dev_attr_batt_read_raw_soc);
	device_remove_file(dev,&dev_attr_batt_capacity_max);
}
static int a52h_attrs_add(struct platform_device *pdev,
			   struct power_supply *psy)
{
	int rc;
	rc=device_create_file(&psy->dev,&dev_attr_batt_capacity_max);
	if (rc)return rc;
	rc=device_create_file(&psy->dev,&dev_attr_batt_read_raw_soc);
	if (rc) {
		device_remove_file(&psy->dev,&dev_attr_batt_capacity_max);
		return rc;
	}
	rc=device_create_file(&psy->dev,&dev_attr_batt_full_capacity);
	if (rc) {
		device_remove_file(&psy->dev,&dev_attr_batt_read_raw_soc);
		device_remove_file(&psy->dev,&dev_attr_batt_capacity_max);
		return rc;
	}
	rc=devm_add_action_or_reset(&pdev->dev,a52h_attrs_release,&psy->dev);
	return rc;
}

static void a52e_put_fg(void *arg)
{
	power_supply_put(arg);
}

/* A52_PHASE447H_NTC_RETRY_V1
 * The platform driver's ->remove synchronously cancels work BEFORE its
 * devres resources, including the IIO channel, are released.
 */
static void a52h_ntc_retry_work(struct work_struct *work)
{
	struct a52e_battery *bat=
		container_of(to_delayed_work(work),struct a52e_battery,a52h_ntc_retry);
	struct a52d_fg *fg=power_supply_get_drvdata(bat->fg);
	int rc=a52g_ntc_attach(bat->a52h_pdev,fg);
	int crc=a52h_chg_ntc_attach(bat->a52h_pdev,fg);
	if (!rc && !crc) {
		dev_info(&bat->a52h_pdev->dev,
			 "P447H NTC READY retries=%u source=thermistor\n",
			 bat->a52h_attempts);
		return;
	}
	if (!(bat->a52h_attempts++ % 15))
		dev_info(&bat->a52h_pdev->dev,
			 "P447H NTC RETRY rc=%d chg_rc=%d attempts=%u interval_ms=2000 src=FG_FALLBACK\n",
			 rc,crc,bat->a52h_attempts);
	schedule_delayed_work(&bat->a52h_ntc_retry,msecs_to_jiffies(2000));
}
static int a52h_battery_remove(struct platform_device *pdev)
{
	struct a52e_battery *bat=platform_get_drvdata(pdev);
	if (bat)
		cancel_delayed_work_sync(&bat->a52h_ntc_retry);
	return 0;
}

static int a52e_battery_platform_probe(struct platform_device *pdev)
{
	struct a52e_battery *bat;
	struct power_supply *psy;
	struct power_supply_config cfg={};
	union power_supply_propval cap={};
	char *path;
	int ret;

	if (!a52_447_enabled(A52_447_BATTERY))
		return -ENODEV;
	/* Exact DT compatible used by TG's production sec_battery. */
	if (!of_device_is_compatible(pdev->dev.of_node,"samsung,sec-battery"))
		return -ENODEV;
	bat=devm_kzalloc(&pdev->dev,sizeof(*bat),GFP_KERNEL);
	if (!bat)return -ENOMEM;
	mutex_init(&bat->a52h_cap_lock);
	bat->a52h_cap_nominal=1000;
	bat->a52h_cap_min=0;
	bat->a52h_cap_margin=300;
	bat->a52h_cap_max=1000;
	of_property_read_u32(pdev->dev.of_node,"battery,battery_full_capacity",
			     (u32 *)&bat->a52h_nominal_full_mah);
	bat->a52h_pdev=pdev;
	INIT_DELAYED_WORK(&bat->a52h_ntc_retry,a52h_ntc_retry_work);
	platform_set_drvdata(pdev,bat);
	bat->fg=power_supply_get_by_name("sm5714-fuelgauge");
	if (!bat->fg)
		return dev_err_probe(&pdev->dev,-EPROBE_DEFER,
				      "P447E battery FG supplier pending\n");
	/* TG has a nominal DT max; a sysfs writer may restore its learned
	 * value only within the TG capacity_max_margin. No guessed 920.
	 */
	{
		struct a52d_fg *fg=power_supply_get_drvdata(bat->fg);
		struct device_node *np=of_find_node_by_name(NULL,"sm5714-fuelgauge");
		bat->a52h_cap_nominal=fg->a52f_capacity_max;
		bat->a52h_cap_min=fg->a52f_capacity_min;
		bat->a52h_cap_max=bat->a52h_cap_nominal;
		if (np) {
			of_property_read_u32(np,"fuelgauge,capacity_max_margin",
					    (u32 *)&bat->a52h_cap_margin);
			of_node_put(np);
		}
		if (bat->a52h_cap_nominal<=bat->a52h_cap_min ||
		    bat->a52h_cap_margin<=0 ||
		    bat->a52h_cap_margin>bat->a52h_cap_nominal)
			return -EINVAL;
	}
	ret=devm_add_action_or_reset(&pdev->dev,a52e_put_fg,bat->fg);
	if (ret)return ret;
	/* P447H: attach only on successfully registered battery platform. */
	ret=power_supply_get_property(bat->fg,POWER_SUPPLY_PROP_CAPACITY,&cap);
	if (ret || cap.intval<0 || cap.intval>100)
		return dev_err_probe(&pdev->dev,-EPROBE_DEFER,
				     "P447E battery FG invalid capacity\n");
	cfg.drv_data=bat;
	cfg.of_node=pdev->dev.of_node;
	psy=devm_power_supply_register(&pdev->dev,&a52e_battery_desc,&cfg);
	if (IS_ERR(psy))
		return dev_err_probe(&pdev->dev,PTR_ERR(psy),
				     "P447E battery platform supply registration failed\n");
	ret=a52h_attrs_add(pdev,psy);
	if (ret)
		return dev_err_probe(&pdev->dev,ret,
			"P447H SOC sysfs attrs registration failed\n");
	dev_info(&pdev->dev,
		 "P447H SOC initialized cap_max=%d margin=%d min=%d writer=default sysfs=batt_capacity_max\n",
		 bat->a52h_cap_max,bat->a52h_cap_margin,bat->a52h_cap_min);
	path=kobject_get_path(&psy->dev.kobj,GFP_KERNEL);
	dev_info(&pdev->dev,
		 "P447E battery PLATFORM_PROBE rc=0 parent=%s fg=sm5714-fuelgauge soc=%d path=%s\n",
		 dev_name(&pdev->dev),cap.intval,path?path:"(OOM)");
	kfree(path);
	ret=a52g_ntc_attach(pdev,power_supply_get_drvdata(bat->fg));
	{
		int chg_rc=a52h_chg_ntc_attach(pdev,power_supply_get_drvdata(bat->fg));
		if (ret<0 || chg_rc<0) {
		dev_info(&pdev->dev,"P447H NTC INITIAL rc=%d chg_rc=%d retry_ms=2000 backup=FG\n",ret,chg_rc);
		schedule_delayed_work(&bat->a52h_ntc_retry,msecs_to_jiffies(2000));
		}
	}
	return 0;
}
static const struct of_device_id a52e_battery_of_match[]={
	{.compatible="samsung,sec-battery"},{}
};
MODULE_DEVICE_TABLE(of,a52e_battery_of_match);
static struct platform_driver a52e_battery_platform_driver={
	.probe=a52e_battery_platform_probe,
	.remove=a52h_battery_remove,
	.driver={
		.name="sec-battery",
		.of_match_table=a52e_battery_of_match,
	},
};

static const struct of_device_id a52d_fg_of_match[]={
	{ .compatible="siliconmitus,sm5714mfd" },{}
};
MODULE_DEVICE_TABLE(of,a52d_fg_of_match);
static const struct i2c_device_id a52d_fg_id[]={
	{"sm5714mfd",0},{}
};
MODULE_DEVICE_TABLE(i2c,a52d_fg_id);
static struct i2c_driver a52d_fg_driver={
	.driver={
		.name="a52-sm5714-ro",
		.of_match_table=a52d_fg_of_match,
	},
	.probe=a52d_fg_probe,
	.id_table=a52d_fg_id,
};
/* Register the I2C supplier first, then the Samsung platform consumer.
 * The platform may also defer until SE7 creates the I2C fuelgauge.
 */
static int __init a52e_fg_init(void)
{
	int rc=i2c_add_driver(&a52d_fg_driver);
	if (rc)return rc;
	rc=platform_driver_register(&a52e_battery_platform_driver);
	if (rc)i2c_del_driver(&a52d_fg_driver);
	return rc;
}
module_init(a52e_fg_init);
static void __exit a52e_fg_exit(void)
{
	platform_driver_unregister(&a52e_battery_platform_driver);
	i2c_del_driver(&a52d_fg_driver);
}
module_exit(a52e_fg_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A52 read-only SM5714 fuel-gauge 5.10 proof of real battery measurement");

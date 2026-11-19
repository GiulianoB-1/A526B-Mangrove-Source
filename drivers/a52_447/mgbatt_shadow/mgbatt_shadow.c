// SPDX-License-Identifier: GPL-2.0-only
/* P477 MGBATT SHADOW stage 1-2. Pure observer called by the EXISTING
 * H0/P449 worker, does not register any I2C client or power_supply.
 */
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/string.h>
#include "mgbatt_shadow.h"

static DEFINE_MUTEX(mgbatt_lock);
static bool attempted, ready;
static int last_dev1 = -999, last_index = -999;
static unsigned long last_report;
static struct mgbatt_policy policy;
void mgbatt_shadow_tick(struct device *owner, const char *type,
			int muic_dev1, int bat, int chg)
{
	int index, rc;
	u32 icl=0,fcc=0;
	const char *reason="UNKNOWN";
	bool report;
	if(!owner) return;
	mutex_lock(&mgbatt_lock);
	if(!attempted) {
		attempted=true;
		rc=mgbatt_parse_live_dt(&policy);
		if(rc) {
			dev_err(owner,"MGBATT SHADOW PROBE ok=0 rc=%d shadow=1\n",rc);
			goto out;
		}
		ready=true;
		dev_info(owner,"MGBATT SHADOW PROBE ok=1 shadow=1 owner=H0\n");
		dev_info(owner,"MGBATT SHADOW WRITE_BASELINE shadow_hw_writes=0 denied=%u\n",
			 mgbatt_write_denials());
	}
	if(!ready) goto out;
	index=mgbatt_cable_index(type);
	/* Real DCP default must be verified, not assumed from MUIC. */
	if(index==3 &&
	   (policy.cable[3].icl_ma!=1800 || policy.cable[3].fcc_ma!=2100))
		index=-1;
	report=(muic_dev1!=last_dev1 || index!=last_index ||
		time_after_eq(jiffies,last_report+60*HZ));
	if(!report) goto out;
	last_dev1=muic_dev1;
	last_index=index;
	last_report=jiffies;
	dev_info(owner,"MGBATT SHADOW CABLE src=muic dev1=0x%02x type=%s pd_valid=0\n",
		 muic_dev1<0?0xff:muic_dev1 & 0xff,
		 type?type:"UNKNOWN");
	mgbatt_policy_calculate(&policy,index,bat,chg,&icl,&fcc,&reason);
	dev_info(owner,"MGBATT SHADOW WOULD_SET cable=%s icl_ma=%u fcc_ma=%u float_mv=%u voters=%s\n",
		 type?type:"UNKNOWN",icl,fcc,policy.float_mv,reason);
	dev_info(owner,"MGBATT SHADOW WRITE_BASELINE shadow_hw_writes=0 denied=%u\n",
		 mgbatt_write_denials());
out:
	mutex_unlock(&mgbatt_lock);
}

/* P479_MGBATT_SHADOW3_COMPARE_NO_WRITES_V1
 * This observer never applies a vote, writes a register, or changes H0.
 * Full/recharge indications are reported using the live r02 thresholds;
 * they are not a second Samsung battery state machine.
 */
void mgbatt_shadow_compare(struct device *owner, const char *type,
                           int bat, int chg, int soc, int vcell,
                           int actual_icl, int actual_fcc, bool full)
{
    static int prev_icl=-1,prev_fcc=-1,prev_wicl=-1,prev_wfcc=-1;
    static bool prev_full;
    static unsigned long last_compare;
    int index;
    u32 would_icl=0,would_fcc=0;
    const char *reason="UNAVAILABLE";
    bool changed;
    if (!owner) return;
    mutex_lock(&mgbatt_lock);
    if (!ready) goto out;
    index=mgbatt_cable_index(type);
    mgbatt_policy_calculate(&policy,index,bat,chg,
                            &would_icl,&would_fcc,&reason);
    /* Real DT full-check/recharge thresholds, observational only.
     * Respect H0's hardware top-off/full state rather than inventing
     * a TG sec_battery vote or changing charging hardware.
     */
    if (full && soc>=0 && vcell>=0) {
        if (soc>=policy.full_soc &&
            vcell>=policy.full_vcell) {
            would_fcc=0;
            reason="TG_R02_FULL_CONDITION";
        } else if (vcell<=policy.recharge_vcell ||
                   soc<=policy.recharge_soc) {
            reason="TG_R02_RECHARGE_CONDITION";
        } else {
            reason="H0_FULL_NO_TG_THRESHOLD_MATCH";
        }
    }
    changed=(actual_icl!=prev_icl || actual_fcc!=prev_fcc ||
             would_icl!=prev_wicl || would_fcc!=prev_wfcc ||
             full!=prev_full);
    if (!changed &&
        time_before(jiffies,last_compare+60*HZ))
        goto out;
    prev_icl=actual_icl;
    prev_fcc=actual_fcc;
    prev_wicl=would_icl;
    prev_wfcc=would_fcc;
    prev_full=full;
    last_compare=jiffies;
    dev_info(owner,
      "MGBATT SHADOW COMPARE would_icl=%u would_fcc=%u actual_icl=%d actual_fcc=%d match=%d reason=%s\n",
      would_icl,would_fcc,actual_icl,actual_fcc,
      would_icl==actual_icl && would_fcc==actual_fcc,reason);
out:
    mutex_unlock(&mgbatt_lock);
}

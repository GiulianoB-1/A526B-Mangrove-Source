// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2012-2019, The Linux Foundation. All rights reserved.
 * P483: TG 6bf351bd controls, native 5.10 component-only adaptation.
 * No PCM callbacks, DAI, call start/stop or call calibration allocation.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/atomic.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/control.h>
#include "../include/dsp/q6voice.h"
#define DRV_NAME "msm-pcm-voice-v2"
#define NUM_CHANNELS_MONO 1
#define NUM_CHANNELS_STEREO 2

static int msm_voice_sidetone_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int ret;
	long value = ucontrol->value.integer.value[0];
	bool sidetone_enable = value;
	uint32_t session_id = ALL_SESSION_VSID;

	if (value < 0) {
		pr_err("%s: Invalid arguments sidetone enable %ld\n",
			 __func__, value);
		ret = -EINVAL;
		return ret;
	}
	ret = voc_set_afe_sidetone(session_id, sidetone_enable);
	pr_debug("%s: AFE Sidetone enable=%d session_id=0x%x ret=%d\n",
		 __func__, sidetone_enable, session_id, ret);
	return ret;
}

static int msm_voice_sidetone_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = voc_get_afe_sidetone();
	return 0;
}

static int msm_voice_gain_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	int volume = ucontrol->value.integer.value[0];
	uint32_t session_id = ucontrol->value.integer.value[1];
	int ramp_duration = ucontrol->value.integer.value[2];

	if ((volume < 0) || (ramp_duration < 0)
		|| (ramp_duration > MAX_RAMP_DURATION)) {
		pr_err(" %s Invalid arguments", __func__);

		ret = -EINVAL;
		goto done;
	}

	pr_debug("%s: volume: %d session_id: %#x ramp_duration: %d\n", __func__,
		volume, session_id, ramp_duration);

	voc_set_rx_vol_step(session_id, RX_PATH, volume, ramp_duration);

done:
	return ret;
}

static int msm_voice_mute_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	int mute = ucontrol->value.integer.value[0];
	uint32_t session_id = ucontrol->value.integer.value[1];
	int ramp_duration = ucontrol->value.integer.value[2];

	if ((mute < 0) || (mute > 1) || (ramp_duration < 0)
		|| (ramp_duration > MAX_RAMP_DURATION)) {
		pr_err(" %s Invalid arguments", __func__);

		ret = -EINVAL;
		goto done;
	}

	pr_debug("%s: mute=%d session_id=%#x ramp_duration=%d\n", __func__,
		mute, session_id, ramp_duration);

	ret = voc_set_tx_mute(session_id, TX_PATH, mute, ramp_duration);

done:
	return ret;
}

static int msm_voice_tx_device_mute_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	int mute = ucontrol->value.integer.value[0];
	uint32_t session_id = ucontrol->value.integer.value[1];
	int ramp_duration = ucontrol->value.integer.value[2];

	if ((mute < 0) || (mute > 1) || (ramp_duration < 0) ||
	    (ramp_duration > MAX_RAMP_DURATION)) {
		pr_err(" %s Invalid arguments", __func__);

		ret = -EINVAL;
		goto done;
	}

	pr_debug("%s: mute=%d session_id=%#x ramp_duration=%d\n", __func__,
		 mute, session_id, ramp_duration);

	ret = voc_set_device_mute(session_id, VSS_IVOLUME_DIRECTION_TX,
				  mute, ramp_duration);

done:
	return ret;
}

static int msm_voice_rx_device_mute_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	int mute = ucontrol->value.integer.value[0];
	uint32_t session_id = ucontrol->value.integer.value[1];
	int ramp_duration = ucontrol->value.integer.value[2];

	if ((mute < 0) || (mute > 1) || (ramp_duration < 0) ||
	    (ramp_duration > MAX_RAMP_DURATION)) {
		pr_err(" %s Invalid arguments", __func__);

		ret = -EINVAL;
		goto done;
	}

	pr_debug("%s: mute=%d session_id=%#x ramp_duration=%d\n", __func__,
		 mute, session_id, ramp_duration);

	voc_set_device_mute(session_id, VSS_IVOLUME_DIRECTION_RX,
			    mute, ramp_duration);

done:
	return ret;
}

static int msm_voice_mbd_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = voc_get_mbd_enable();
	return 0;
}

static int msm_voice_mbd_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	bool enable = ucontrol->value.integer.value[0];

	voc_set_mbd_enable(enable);

	return 0;
}


static const char * const tty_mode[] = {"OFF", "HCO", "VCO", "FULL"};
static const struct soc_enum msm_tty_mode_enum[] = {
		SOC_ENUM_SINGLE_EXT(4, tty_mode),
};

static int msm_voice_tty_mode_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] =
		voc_get_tty_mode(voc_get_session_id(VOICE_SESSION_NAME));
	return 0;
}

static int msm_voice_tty_mode_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	int tty_mode = ucontrol->value.integer.value[0];

	pr_debug("%s: tty_mode=%d\n", __func__, tty_mode);

	voc_set_tty_mode(voc_get_session_id(VOICE_SESSION_NAME), tty_mode);
	voc_set_tty_mode(voc_get_session_id(VOICE2_SESSION_NAME), tty_mode);
	voc_set_tty_mode(voc_get_session_id(VOLTE_SESSION_NAME), tty_mode);
	voc_set_tty_mode(voc_get_session_id(VOWLAN_SESSION_NAME), tty_mode);
	voc_set_tty_mode(voc_get_session_id(VOICEMMODE1_NAME), tty_mode);
	voc_set_tty_mode(voc_get_session_id(VOICEMMODE2_NAME), tty_mode);

	return 0;
}

static int msm_voice_slowtalk_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int st_enable = ucontrol->value.integer.value[0];
	uint32_t session_id = ucontrol->value.integer.value[1];
	struct module_instance_info mod_inst_info;

	memset(&mod_inst_info, 0, sizeof(mod_inst_info));
	pr_debug("%s: st enable=%d session_id=%#x\n", __func__, st_enable,
		 session_id);

	mod_inst_info.module_id = MODULE_ID_VOICE_MODULE_ST;
	mod_inst_info.instance_id = INSTANCE_ID_0;
	voc_set_pp_enable(session_id, mod_inst_info, st_enable);

	return 0;
}

static int msm_voice_ecns_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	uint32_t enable = ucontrol->value.integer.value[0];
	uint32_t session_id = ucontrol->value.integer.value[1];
	uint32_t module_id = ucontrol->value.integer.value[2];

	pr_debug("%s: ecns enable=%d session_id=%#x\n", __func__, enable,
		 session_id);
	voc_set_ecns_enable(session_id, module_id, enable);

	return 0;
}

static int msm_voice_hd_voice_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	uint32_t hd_enable = ucontrol->value.integer.value[0];
	uint32_t session_id = ucontrol->value.integer.value[1];

	pr_debug("%s: HD Voice enable=%d session_id=%#x\n", __func__, hd_enable,
		 session_id);

	ret = voc_set_hd_enable(session_id, hd_enable);

	return ret;
}

static int msm_voice_topology_disable_put(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	int disable = ucontrol->value.integer.value[0];
	uint32_t session_id = ucontrol->value.integer.value[1];

	if ((disable < 0) || (disable > 1)) {
		pr_err(" %s Invalid arguments: %d\n", __func__, disable);

		ret = -EINVAL;
		goto done;
	}
	pr_debug("%s: disable = %d, session_id = %d\n", __func__, disable,
		 session_id);

	ret = voc_disable_topology(session_id, disable);

done:
	return ret;
}

static int msm_voice_rec_config_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	int ret = 0;
	int voc_rec_config_channels = ucontrol->value.integer.value[0];

	if (voc_rec_config_channels < NUM_CHANNELS_MONO ||
			voc_rec_config_channels > NUM_CHANNELS_STEREO) {
		pr_err("%s: Invalid channel config (%d)\n", __func__,
			voc_rec_config_channels);
		ret = -EINVAL;
		goto done;
	}
	voc_set_incall_capture_channel_config(voc_rec_config_channels);

done:
	pr_debug("%s: voc_rec_config_channels = %d, ret = %d\n", __func__,
		voc_rec_config_channels, ret);
	return ret;
}

static int msm_voice_rec_config_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{

	ucontrol->value.integer.value[0] =
		voc_get_incall_capture_channel_config();
	pr_debug("%s: rec_config_channels = %ld\n", __func__,
		ucontrol->value.integer.value[0]);
	return 0;
}

static int msm_voice_cvd_version_info(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_info *uinfo)
{
	int ret = 0;

	pr_debug("%s:\n", __func__);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = CVD_VERSION_STRING_MAX_SIZE;

	return ret;
}

static int msm_voice_cvd_version_get(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	char cvd_version[CVD_VERSION_STRING_MAX_SIZE] = CVD_VERSION_DEFAULT;
	int ret;
	static atomic_t logged = ATOMIC_INIT(0);

	ret = voc_get_cvd_version(cvd_version);
	if (ret)
		strscpy(cvd_version, CVD_VERSION_DEFAULT, sizeof(cvd_version));
	cvd_version[sizeof(cvd_version) - 1] = '\0';
	if (atomic_cmpxchg(&logged, 0, 1) == 0)
		pr_info("P483 CVD_VERSION value=%s rc=%d\n", cvd_version, ret);

	if (ret)
		pr_err("%s: Error retrieving CVD version, error:%d\n",
			__func__, ret);

	memcpy(ucontrol->value.bytes.data, cvd_version, sizeof(cvd_version));

	return 0;
}
static struct snd_kcontrol_new msm_voice_controls[] = {
	SOC_SINGLE_MULTI_EXT("Voice Rx Device Mute", SND_SOC_NOPM, 0, VSID_MAX,
				0, 3, NULL, msm_voice_rx_device_mute_put),
	SOC_SINGLE_MULTI_EXT("Voice Tx Device Mute", SND_SOC_NOPM, 0, VSID_MAX,
				0, 3, NULL, msm_voice_tx_device_mute_put),
	SOC_SINGLE_MULTI_EXT("Voice Tx Mute", SND_SOC_NOPM, 0, VSID_MAX,
				0, 3, NULL, msm_voice_mute_put),
	SOC_SINGLE_MULTI_EXT("Voice Rx Gain", SND_SOC_NOPM, 0, VSID_MAX, 0, 3,
				NULL, msm_voice_gain_put),
	SOC_ENUM_EXT("TTY Mode", msm_tty_mode_enum[0], msm_voice_tty_mode_get,
				msm_voice_tty_mode_put),
	SOC_SINGLE_MULTI_EXT("Slowtalk Enable", SND_SOC_NOPM, 0, VSID_MAX, 0, 2,
				NULL, msm_voice_slowtalk_put),
	SOC_SINGLE_MULTI_EXT("Voice ECNS Enable", SND_SOC_NOPM, 0, VSID_MAX, 0, 3,
				NULL, msm_voice_ecns_put),
	SOC_SINGLE_MULTI_EXT("Voice Topology Disable", SND_SOC_NOPM, 0,
			     VSID_MAX, 0, 2, NULL,
			     msm_voice_topology_disable_put),
	SOC_SINGLE_MULTI_EXT("HD Voice Enable", SND_SOC_NOPM, 0, VSID_MAX, 0, 2,
			     NULL, msm_voice_hd_voice_put),
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "CVD Version",
		.info	= msm_voice_cvd_version_info,
		.get	= msm_voice_cvd_version_get,
	},
	SOC_SINGLE_MULTI_EXT("Voice Sidetone Enable", SND_SOC_NOPM, 0, 1, 0, 1,
			     msm_voice_sidetone_get, msm_voice_sidetone_put),
	SOC_SINGLE_BOOL_EXT("Voice Mic Break Enable", 0, msm_voice_mbd_get,
				msm_voice_mbd_put),
};

static struct snd_kcontrol_new msm_voice_rec_config_controls[] = {
	SOC_SINGLE_MULTI_EXT("Voc Rec Config", SND_SOC_NOPM, 0,
			     2, 0, 1, msm_voice_rec_config_get,
			     msm_voice_rec_config_put),
};


static int msm_pcm_voice_probe(struct snd_soc_component *component)
{
	int rc;

	rc = snd_soc_add_component_controls(component, msm_voice_controls,
					    ARRAY_SIZE(msm_voice_controls));
	if (rc)
		return rc;
	rc = snd_soc_add_component_controls(component, msm_voice_rec_config_controls,
					    ARRAY_SIZE(msm_voice_rec_config_controls));
	dev_info(component->dev, "P483 VOICE_KCONTROLS count=%zu rc=%d\n",
		 ARRAY_SIZE(msm_voice_controls) + ARRAY_SIZE(msm_voice_rec_config_controls), rc);
	return rc;
}

static const struct snd_soc_component_driver msm_soc_component = {
	.name = DRV_NAME,
	.probe = msm_pcm_voice_probe,
};

static int msm_pcm_probe(struct platform_device *pdev)
{
	int rc;

	if (!is_voc_initialized())
		return -EPROBE_DEFER;
	voc_set_destroy_cvd_flag(of_property_read_bool(pdev->dev.of_node, "qcom,destroy-cvd"));
	rc = devm_snd_soc_register_component(&pdev->dev, &msm_soc_component, NULL, 0);
	dev_info(&pdev->dev, "P483 VOICE_PLATFORM probe rc=%d\n", rc);
	return rc;
}

static const struct of_device_id msm_voice_dt_match[] = {
	{ .compatible = "qcom,msm-pcm-voice" },
	{}
};
MODULE_DEVICE_TABLE(of, msm_voice_dt_match);

static struct platform_driver msm_pcm_driver = {
	.driver = {
		.name = "msm-pcm-voice",
		.owner = THIS_MODULE,
		.of_match_table = msm_voice_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = msm_pcm_probe,
};

int __init msm_pcm_voice_init(void)
{
	return platform_driver_register(&msm_pcm_driver);
}

void msm_pcm_voice_exit(void)
{
	platform_driver_unregister(&msm_pcm_driver);
}
MODULE_DESCRIPTION("P483 TG voice controls only, no voice PCM");
MODULE_LICENSE("GPL v2");

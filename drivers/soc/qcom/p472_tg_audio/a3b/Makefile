# P472_A3B_LPASS_CODEC_REAL_D54_V1
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/a3b/include
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/a3b/include/uapi
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/a3b/include/uapi/audio
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/include
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/include/uapi
subdir-ccflags-$(CONFIG_P472_A3B_B1) += -DCONFIG_PINCTRL_WCD=1 -DCONFIG_PINCTRL_LPI=1 -DCONFIG_MSM_CDC_PINCTRL=1
subdir-ccflags-$(CONFIG_P472_A3B_B2) += -DCONFIG_SND_SOC_BOLERO=1 -DCONFIG_RX_MACRO=1 -DCONFIG_TX_MACRO=1 -DCONFIG_VA_MACRO=1
subdir-ccflags-$(CONFIG_P472_A3B_B3) += -DCONFIG_SOUNDWIRE=1 -DCONFIG_SOUNDWIRE_WCD_CTRL=1 -DCONFIG_SOUNDWIRE_MSTR_CTRL=1 -DCONFIG_SND_SOC_WCD938X=1 -DCONFIG_SND_SOC_WCD938X_SLAVE=1 -DCONFIG_WCD9XXX_CODEC_CORE_V2=1 -DCONFIG_SND_SOC_WCD9XXX_V2=1 -DCONFIG_SND_SOC_WCD_MBHC_ADC=1 -DCONFIG_SND_SOC_WCD_MBHC=1 -DCONFIG_WCD9XXX_CODEC_CORE=1
# TG lito_a52.h: enable the complete Samsung impedance event/volume path.
subdir-ccflags-$(CONFIG_P480_A3C) += -DCONFIG_SND_SOC_IMPED_SENSING=1
obj-y += soc/ asoc/

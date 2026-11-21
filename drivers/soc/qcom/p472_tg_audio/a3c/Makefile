# P480_A3C_NATIVE_WCD938X_KONA_V1
# A3c-specific real donor ION declarations precede public A3b includes.
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/a3c/include
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/a3b/include
# The pinned A3b donor carries TG/QCOM UAPI headers in include/uapi/audio/.
# Add its root so <audio/linux/msm_audio.h> and
# <audio/sound/audio_effects.h> resolve to genuine donor declarations.
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/a3b/include/uapi
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/a3b/include/uapi/audio
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/include
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/include/uapi
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/a3c
subdir-ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/a3c/codecs
subdir-ccflags-y += -DCONFIG_SND_SOC_QDSP6V2=1
# The genuine msm-qti-pp-config.o is built into A3c. Its donor header must
# expose the matching real functions, not the CONFIG_QTI_PP=n inline stubs.
# Scope this feature selection to A3c only and keep P480_A3C default n.
subdir-ccflags-y += -DCONFIG_QTI_PP=1
# A3b builds the genuine CDC pinctrl provider. Its pinned donor header
# otherwise emits global fallback definitions when CONFIG_MSM_CDC_PINCTRL
# is absent, colliding with that same provider at vmlinux.o link time.
# Select the real external prototypes for this A3c directory only.
subdir-ccflags-y += -DCONFIG_MSM_CDC_PINCTRL=1
# Kona's reachable TG RX init must call the genuine A3b bolero API,
# not the disabled-config inline success stubs in its donor header.
CFLAGS_kona.o += -DCONFIG_SND_SOC_BOLERO=1
obj-$(CONFIG_P480_A3C) += msm-pcm-q6-v2.o msm-compress-q6-v2.o
obj-$(CONFIG_P480_A3C) += msm-pcm-hostless.o msm-pcm-afe-v2.o msm-pcm-q6-noirq.o
obj-$(CONFIG_P480_A3C) += msm-pcm-routing-v2.o msm-pcm-routing-devdep.o
obj-$(CONFIG_P480_A3C) += msm-dai-q6-v2.o msm-dai-fe.o msm-dai-stub-v2.o
obj-$(CONFIG_P480_A3C) += msm-audio-effects-q6-v2.o msm-qti-pp-config.o
obj-$(CONFIG_P480_A3C) += platform_init.o kona.o codecs/

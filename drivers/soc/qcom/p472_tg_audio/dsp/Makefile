ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/include -I$(srctree)/drivers/soc/qcom/p472_tg_audio/include/uapi -I$(srctree)/drivers/soc/qcom/p472_tg_audio/dsp
obj-y += audio_ssr.o audio_notifier.o
# Kbuild extra-y compiles q6core.o but does NOT link it into built-in.a.
# Required for the native probe while P472_TG_Q6CORE remains disabled.
extra-y += q6core.o
obj-$(CONFIG_P472_TG_Q6CORE) += q6core.o

# A3a: TG DSP Kbuild:114-130, excluding already-linked A2a and
# original q6_init.c (would reinitialize A2a cal/q6core/ION).
obj-$(CONFIG_P472_A3A_Q6) += q6audio-v2.o q6common.o q6adm.o q6afe.o q6asm.o q6lsm.o
obj-$(CONFIG_P472_A3A_Q6) += q6voice.o rtac.o msm-audio-event-notify.o avtimer.o p472_a3a_q6_init.o

# A2a: link real TG q6core/calibration via existing 5.10 ION
obj-$(CONFIG_P472_TG_Q6CORE) += adsp_err.o audio_calibration.o audio_cal_utils.o msm_audio_ion.o p472_a2a_core_init.o

# P482: A3b selects the real TG hardware vote backend.
obj-$(CONFIG_P472_A3B_B1) += digital-cdc-rsc-mgr.o

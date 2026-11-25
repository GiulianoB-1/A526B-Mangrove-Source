# P487 compile-only native flags; no private CONFIG definitions.
ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/a3b/asoc/codecs/tas256x
ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/include
ccflags-y += -I$(srctree)/drivers/soc/qcom/p472_tg_audio/a3b/include
ccflags-y += -I$(srctree)/a52-compat/include
obj-$(CONFIG_SND_SOC_TAS256x) += tas256x-review.o
tas256x-review-y := os_layer/src/tas256x-codec.o os_layer/src/tas256x-regmap.o \
    physical_layer/src/tas256x.o logical_layer/src/tas256x-logic.o \
    algo/src/tas25xx-algo-common.o algo/src/tas25xx-calib.o \
    algo/src/tas25xx-algo-intf.o algo/platform/qcom/tas25xx-algo-qdsp-intf.o \
    misc/tas256x-misc.o algo/src/tas25xx-calib-validation.o
# Samsung bigdata callback is imported but not linked against fallback stubs.

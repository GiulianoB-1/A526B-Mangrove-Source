# P487 staged cleanup (not final P487, NO FLASH)

Base: hardware-verified P486 merged into main at cbc904fbd2a6178b440108eff4bfca72dcc4d347.

## Implemented in this commit

- a52_audio.debug=0 by default and /sys/kernel/a52_audio/debug (RW); non-A3c builds retain the original logging. Gate TX_SEQ (negative returns always logged), TX_DAPM, WCD/SWR IRQ trace, SWR wake trace, TX_CHMAP, MBHC event trace, CDC_DMA vote success, LPASS vote success. Preserve actual hardware operations, milestone lines, error paths and explicit MBHC register dumps. No global ASoC edits.
- Separate work items identify actual manual knob vs APR event. Both use the same mutex/one-shot. Retry only -EAGAIN/-EPROBE_DEFER returned by platform_driver_register, at most three retries at 2 s. Successful driver registration is never repeated. Driver-core deferred device probe remains untouched. Stop retry if APR is no longer LOADED.
- Update readiness audit for the actual work paths and retry bound.
- LOG_BUF_SHIFT is already 23, so unchanged.
- ADC2 source: shift=1, ADC2 REQ shift=1, ADC2_MIXER wcd938x_enable_req; same callback remap as ADC1 with existing BCS/MBHC code. Not headset-mic hardware proof.

## Donor evidence / incomplete work

TG micr0softstore/samsung_android_kernel_a52xq@6bf351bd: techpack/audio/asoc/codecs/tas256x/ (27 files), Kbuild and techpack/audio/config/lito_a52.conf + lito_a52.h enable SND_SOC_TAS256x, TAS25XX_ALGO, PLATFORM_QCOM, TAS256X_REGMAP, TAS256X_CODEC, TAS256X_MISC, TAS25XX_CALIB_VAL_BIG. Kconfig spells SND_SOC_TAS256X uppercase and incorrectly prefixes CONFIG_TAS25XX_CALIB_VAL_BIG; reconcile to exact consumers, not a private define.

No speaker enable, no QUIN filter changes, no calibration access, no DT guesses. Need live R02 merged DT and ROM mixer paths to prove bus/pins/receiver mapping. Donor algo/src/tas25xx-calib-validation.c uses read-only filp_open and legacy set_fs, has factory calibration/validation sysfs stores; algo/src/tas25xx-calib.c has userspace write and ioctl; misc/tas256x-misc.c has raw register writes. Must review all before enabling. AFE TI path from existing q6afe conditional; no mainline TAS substitute.

Autostart candidate now implemented using the exact P458 SCM runtime and P470 IPA one-shot functions and the registered SM6350 PAS remoteproc. Firmware is opened O_RDONLY and one byte read, minimum 12s uptime, poll 200ms through 120s. Hold wakeup source through SCM, 1s, IPA, 1s, ADSP. IPA arm caches actual reprobe result under a shared mutex; sysfs remains CAP_SYS_ADMIN protected. ADSP worker and old sysfs share a serialized start; repeated starts do not increment power. Kill switches a52.autostart=0 and a52.autostart_ipa=0. Modem path and 15s IPA wait untouched. rproc_boot uses native synchronous PAS/firmware timeouts; bounded lookup and RUNNING poll do not cancel an in-flight secure-world call. Firmware reachability in kernel worker mount context, real boot timing and sensor enumeration require phone logs. Old module may log EALREADY/EBUSY and its script may abort; exact module script/logs unavailable. FastRPC audio PD not changed without daemon logs. No claim of hardware success.

Storage: TG and current configs have QUOTA, QFMT_V2, QUOTACTL, FS_ENCRYPTION_INLINE_CRYPT, FUSE_FS enabled. TG QUOTA_NETLINK_INTERFACE=y vs current off; no evidence this causes FS_IOC_FSSETXATTR EACCES. No speculative config change. FUSE passthrough/project quota/casefold implementation and ROM paths need further audit.

This is partial P487, not a final P487 candidate. Do not flash. Only a distinctly named STAGED build is requested for source review; no final P487 default/NOAUTO image pair yet.

## R02 donor board mapping (source evidence, not live verification)

Pinned TG overlay: arch/arm64/boot/dts/samsung/a52/a52xq/a52xq_eur_open_w00_r02.dts, blob 72de76f7bae4b29ccc8663013936c0667a954180. Fragments 122, 124-126, 128 and the overlay fixups establish the following:
- /soc/i2c@18 is an i2c-gpio software adapter with alias i2c18 and cell-index 0x12, not a QUP controller. TLMM SDA/SCL offsets 52/53, donor delay property #i2c-gpio,delay-us=2.
- tas256x@4c: compatible "ti, tas256x", left 0x4d, right 0x4c, channels=2, TLMM reset=55, IRQ=54, iv-width=8, vbat-mon=1, port_id=0x1016.
- I2C pinctrl default references 0x7e (TLMM GPIO52-55, GPIO function, bias disabled) and 0x7f (FM LNA GPIO101, output-low, bias disabled). Do not alter FM LNA incidentally.
- QUIN MI2S pinctrl references 0x81, msm_cdc_pinctrl_quin, compatible qcom,msm-cdc-pinctrl, qcom,lpi-gpios, aud_active/aud_sleep. LPI GPIO6 SCK, GPIO7 WS, GPIO8 SD0, GPIO9 SD1, func2. Active states drive-strength 4, bias disabled, output-high; sleep states drive-strength 2, pull-down, input-enable. Distinct from TLMM amplifier control pins.
- Sound overlay sets qcom,mi2s-audio-intf=1 and qcom,quin-mi2s-gpios=<0x81>. The existing ci/P480-live-r02-merged-sound-evidence.json establishes the card model and routes but does not capture the amplifier bus/pinctrl. No new DT modification yet.
- Stored calibration input names from algo/src/tas25xx-calib-validation.h: /efs/tas25xx/calib_re, amb_temp, calib_re_r, amb_temp_r. Kernel read/apply only; factory sysfs store and misc write/ioctl paths need explicit rejection; no donor code is enabled in this stage. Need port legacy set_fs to kernel_read, review every factory and raw register path, and prove smart-amp protections active before enabling QUIN.
- Earpiece channel/route still requires exact ROM mixer_paths XML, not an assumption from left/right addresses.

The native build for source commit 064302423ffbfbc425cb04c628fda077f9f18270 is run 45632533043; preflight passed at the time of inspection, compile underway. This documentation-only supplement does not change the built source or trigger another native build.

# P487 compile review follow-up: TAS policy (not runtime enabled)

TG original parser: techpack/audio/asoc/codecs/tas256x/algo/src/tas25xx-calib.c at 6bf351bd.
It calculates index=(page-1)*30+(offset-8)/4, length=count/4 (u8 count), accepts non-special index 0..2048 or special 3810..3898 inclusive (including undefined IDs), and packs index | (length<<16) | (channel<<28). It limits length <14 words, but lacks an exact count check, copies the full 60-byte packet regardless of count, ignores book and does not reject an invalid device_id before dispatch. TG passes SET and GET via TISA_MOD_RX, DT port 0x1016. None of these loose checks is an approved runtime allowlist.

| Index / group | Current /dev/tas_calib SET policy | Kernel path |
| --- | --- | --- |
| 0..2048 opaque algorithm settings | Deny pending actual HAL trace | Not authorized |
| 3816 SET_RE, 3823 SET_TCAL | Deny pending writer/payload evidence | Read-only EFS and real AFE SET at unmute |
| 3825 IV_VBAT_FMT, 3837 PCM_CHANNEL_MAPPING, 3840 IV_WIDTH_VBAT_MON | Deny pending trace | Existing TG internal stream path |
| 3819 SET_PROFILE, 3822 SET_SPKID, 3836 SET_INTERFACE_MODE, 3839 DIGITAL_GAIN | Deny pending trace | TG common digital-gain control separate |
| 3814/3815 CALIB_INIT/DEINIT, 3817/3818 F0_TEST_INIT/DEINIT, 3831/3832 VALID_INIT/DEINIT | Deny; factory triggers | No user factory trigger |
| 3834 SET_BYPASS_MODE | Deny; protection bypass | Receiver hardware sequence is separate |
| 3810..3813, 3821, 3824, 3833, 3835, 3838 GET/telemetry IDs | Deny SET | GET path distinct; ABI hardening pending |
| All unspecified/reserved IDs | Deny | No assumed allowlist |
| /dev/tas256x raw write | EPERM | No raw userspace register poke |

Actual userspace writer identity and payload list are not present in the review. No assertion that a permanent blanket deny is safe; runtime enable stays blocked until trace is supplied. In particular do not approve all indices 0..2048 based solely on TG's broad parser.

## Calibration source and status

tas256x_mute_stream(playback unmute) -> tas25xx_send_channel_mapping -> tas25xx_send_algo_calibration -> tas25xx_get_efs_data -> filp_open(O_RDONLY,0), kernel_read, filp_close -> tas25xx_smartamp_algo_ctrl -> tas25xx_smartamp_get_set -> afe_tas_smartamp_set_calib_data -> q6afe_set_params. Both channels' Re/temp are read before SET. Preserve TG Q19 representation (hundredths), reject bad parse or zero/overflow Re. Both TCAL and RE must succeed per channel. Then send IV format via both original parameters. Failures propagate and common controls are disabled; no cached success from prior ADSP instance. Global bypass returns EOPNOTSUPP instead of success. QDSP module range corrected to exclude TISA_MOD_END.

P487 TAS STREAM algo=param_ack means these parameter SET APIs returned success; it is NOT proof of active DSP topology or actual I/V feedback. calib_valid means both files parsed and both channels' SETs acknowledged, not acoustic safety certification. iv_port=0x1017 is the expected TX port, not proof of running capture. TG DAPM can power the amp before mute_stream; this is not a sufficient power interlock yet. Need actual AFE feedback state, power ordering, receiver transition isolation and fault/probe unwind review before enabling. No receiver bypass marker in this stage, no runtime image.

Samsung bigdata source is not in the compile object list, and its registration callback is not called by imported codec. Existing a52-compat/include/sound/samsung/sec_audio_sysfs.h under !CONFIG_SND_SOC_SAMSUNG_AUDIO returns -EACCES for five optional registration calls; donor register_tas25xx_bigdata_cb is void and ignores those return codes, but it is NOT built or invoked here. Internal tas25xx_algo_add_calib_valid_bigdata remains distinct and needed for stored calibration. Do not fake Samsung provider.

B2/C2 and final P487/NOAUTO remain pending. NO FLASH.

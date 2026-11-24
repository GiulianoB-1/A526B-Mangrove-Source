# P481 crash diagnosis: review-only candidate, NO FLASH

Base: a85866eb110fc54e0652c9974674ed40df326839 (P480 Fix 4). User-provided hardware result: card registers with 48 links / 42 skipped, 14 routes, 48 PCM, 8 compress, 6810 controls, followed by abrupt fatal NOC about four seconds later even without an active vendor HAL. This is consistent with, but does not prove, unclocked access. No topology changes.

## Important desk findings
- Both pinned donor digital-cdc-rsc-mgr headers have disabled-config success/no-op functions. The A3b Kbuild flags and ci/build.config do not enable CONFIG_DIGITAL_CDC_RSC_MGR. Thus existing local counts are not proof of hardware votes. The A3b header logs the existing stub backend and real clock-framework reference count; no new simulated votes and no enabling clocks through a guessed API. Positive CCF enable count is necessary evidence, not proof that a physical domain is safe.
- Kona selects real bolero API via its per-file flag, but its WCD938x header still selects disabled-config inline functions at the existing baseline. The real A3b wcd_mbhc_init runs at component probe; a log of that init is not proof that wcd_mbhc_start ran. Do not silently switch to real WCD938x machine-side startup in this diagnostic.
- SWR resume legitimately accesses its registers before state changes to UP. Legacy swr_master_read returns u32 and swr_master_write returns void, and callers discard errors. A blanket guard there would substitute fake read data. P481 guards error-returning slave transactions and PM transitions; **raw master access coverage is incomplete**. Review and complete error propagation before relying on this as a hardware protection build. The existing direct AHB checks are not a proof of safe access.
- Bolero parent and WCD codec/slaves do not have independent runtime suspend/resume callbacks in this tree; the shared bolero callbacks implement the VA/TX/RX macros. RPM_NO_PM means the knob cannot hold a device that has no enabled runtime PM. PM active state is not proof of clocks.
- The DAPM option overrides idle_bias_off in the actual per-card and per-component contexts (effective idle_bias_on), not the const driver descriptors. It holds idle bias in standby; it does NOT prohibit all bias transitions, stream events or system suspend.

## Knobs
After Q6/platform providers register, /sys/kernel/a52_audio is created. Default card not registered. card_enable mode 0200 accepts only 1, exactly one attempt; all other knobs mode 0644, immutable after attempt. Defaults mbhc_enable=1, rpm_forbid=0, dapm_idle_off=0. CARD_ENABLE rc is platform-driver registration rc; CARD register rc separately reports actual card bind. A failure never tears down A3a. RPM hold scans already-bound platform and SoundWire devices and covers late probes. MBHC=0 retains actual jack inputs and inert initialization, skips notifier and IRQ registration, skips start and SSR restart; this also omits MBHC OCP IRQs so **do not use it for audio playback or headset testing**. Diagnostic only. No calibration or mixer writes added. Heartbeat only starts on successful registration and schedules fifteen ticks.

## Source map (relative to drivers/soc/qcom/p472_tg_audio unless noted)
| File | Lines / function |
|---|---|
| a3c/p481_diag.c | 48 heartbeat; 79 RPM hold; 122 knob definitions; 126 one-shot store; 166 sysfs registration |
| dsp/p472_a3a_q6_init.c | 103 register diagnostic interface after platform |
| a3c/kona.c | 7475 MBHC start bypass; 8286 heartbeat; 8460 persistent machine entry point |
| a3b/asoc/codecs/bolero/bolero-cdc.c | 102 guard; 140 read; 231 write; 1583 resume; 1640 suspend |
| a3b/soc/pinctrl-lpi.c | 161 read; 207 write; 891 resume; 936 suspend |
| a3b/soc/swr-mstr-ctrl.c | 1018 refusal; 1029 vote check; 1037 transaction check; 1052 read; 1111 write; 1166 bulk; 3211 resume; 3381 suspend |
| a3b/asoc/codecs/wcd-mbhc-v2.c | 1739 start; 1880 init; 2032 skip before IRQ; 2184 deinit |
| a3b/asoc/codecs/wcd-mbhc-adc.c | 76/142 ADC trace; 593 schedule; 792 work; 1159/1258 IRQ |
| a3b/include/dsp/digital-cdc-rsc-mgr.h | 24 trace old stub backend, 34 enable |
| sound/soc/soc-dapm.c (root) | 733 bias trace; 4822 effective idle-bias hold |

Strict additional Bolero/LPI/SWR refusals arm only after explicit card-enable attempt; before activation preserve the cardless boot path as closely as possible, with PM tracing and balanced errors. First five refusal stacks per subsystem, rate-limited markers. MBHC traces are rate-limited entry diagnostics, not a complete workqueue scheduling trace. No claim of boot stability or full guard coverage.

## Packaging
A3C-only MANGROVE-P481-A3C-HLT.boot.img, exact P479 HLT boot template, pinned DTB and ramdisk, original NO-A3C fallback unchanged; compiled marker proof against P479 and fallback. Existing route/direction gates, thermal, SELinux, IPA and charging safety untouched. No flash until review; not hardware authorized.

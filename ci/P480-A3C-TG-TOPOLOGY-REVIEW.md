# P480 A3C: TG lito WCD938x AUX and duplex MI2S topology review

## Scope

Hardware result at A3C commit `86379327` (user report): six MI2S platform probes rc=0, stable Kona link count 54 (36 skipped), but ALSA sound-card registration still deferred. Never treat this build result as card-register success.

This revision applies only to the native P480 A3C source branch. Retains P479 HLT, P479 safety gates, A2a/A3a Q6 ordering and nonfatal A3c failure policy. Build only A3C; **NO FLASH pending Claude review**.

## Root causes confirmed by reported debugfs and pinned TG donor

1. Current SM7325-derived `msm_dailink.h` requested `wcd938x_codec`/`wcd938x_cdc` for RX_CDC_DMA_RX_0..3 and TX_CDC_DMA_TX_3/4, while live A52 exposes `wcd938x-codec` with one DAI `wcd938x-codec`. The pinned TG lito `kona.c` uses only the registered bolero macro DAIs on those BE links. WCD938x is an AUX codec via `qcom,codec-aux-devs`.
2. The SM7325 split MI2S DAI array uses 27 RX/TX entries whereas TG has one duplex DAI per MI2S interface. The reported component registration did not imply correct registered DAIs, and stream channel maxima were inconsistent with live RX/TX masks.
3. The kernel's `struct snd_soc_pcm_stream` does not support donor 4.19 `.aif_name`. This field alone was omitted while porting the rest of TG's MI2S DAI array, validation, port-ID mapping and per-stream state.

## Source changes

- Six retained RX/TX CDC_DMA BE links now have only their TG `bolero_codec` / `rx_macro_rx{1..4}`, `tx_macro_tx{1..2}` DAIs. The three VA CDC_DMA links were already bolero-only and remain so. No device/codec renames, no fabricated `snd-soc-dummy` substitutions.
- `msm_init_aux_dev` reads the live `qcom,codec-aux-devs` phandle list and binds each actual codec `of_node` as an AUX device with the existing TG-derived `msm_aux_codec_init`. WSA AUX stays excluded. Missing AUX phandles fail closed rather than faking a codec.
- TG MI2S DAI IDs, interface stream names, rates/formats, separate `rx_dai` and `tx_dai` state, `msm_mi2s_get_port_id` and `msm_dai_q6_mi2s_get_lineconfig` are used. Source and TG 5.10 API adaptation is limited to dropping unsupported `.aif_name` members.
- Each RX/TX link in `msm_dailink.h` uses the correct common interface CPU name `msm-dai-q6-mi2s.[0..5]`. On this Linux 5.10 kernel device names differ from TG 4.19 names. Kona therefore resolves the actual `qcom,msm-dai-q6-mi2s` DT node by its `qcom,msm-dai-q6-mi2s-dev-id` and sets the link CPU `of_node` with `dai_name=NULL` when the normal asoc-cpu mapping is absent. No `dev_set_name`.
- `MI2S_DAI_REGISTER` now logs each component device, TG DAI name, playback max, capture max and registration rc. Invalid masks still fail closed.
- Quinary MI2S backend links are excluded by stable BE ID with `TAS_NOT_PORTED`. QUAT and SENARY links are kept only if their actual CPU DAIs resolve after DT population, otherwise skipped with `MI2S_CPU_DAI_UNRESOLVED`.
- Original P480 retry-safe devm card, link and component copies, P479/H4 gates and preserved A3a services remain.

## Expected post-review hardware gate

- Six MI2S platform probes rc=0 **and** six duplex TG DAIs recorded with correct RX/TX maximum channel counts.
- The initial sound-card registration returns `CARD register rc=0 name=lito-lagoonmtp-snd-card`, or a logged specific deferred CPU/codec/platform provider is found and fixed. The expected result is **not guaranteed by source-only checks**.
- `/proc/asound/cards` lists the card; MultiMedia/Compress, RX/TX/VA CDC_DMA bolero links are retained. No TAS, WSA, voice/listen, DP, USB or SLIMBUS paths enabled.
- Confirm that WCD938x AUX callback uses the real component safely and MBHC doesn't introduce new errors. Preserve the read-only safety gates.
- Exactly one A3C diagnostic image, P479 HLT provenance and unchanged DTB/ramdisk/cmdline verified; **NO FLASH** until Claude signs off.

References: TG `micr0softstore/samsung_android_kernel_a52xq@6bf351bdf18bdb228db79e66f14a7a9c0178e5d7`, `techpack/audio/asoc/kona.c` and `msm-dai-q6-v2.c`. Hardware evidence is the user's reported debugfs and DT values, not independently measured by CI.

# P480 A3C Fix 3: real RX initialization and exact DAPM exclusions

**NO FLASH until Claude reviews.** Based on hardware-tested source `5d7fbeb811e6b8d4c890272b764d37cebd9a4f2f`.

## Hardware evidence before Fix 3 (user report, not independently acquired)
- Card registration returned -19 at 49.27 s with 48 retained/42 skipped links. Fix 2 direction gate validated: PRI_MI2S_TX and SEC_MI2S_TX absent capture; TERT_MI2S_RX and SENARY_MI2S_RX absent playback; KONA Compress1 capture trimmed.
- Routing read errors = 0; WCD938x codec and AUX already present. WCD MBHC initialized. Missing five Analog Mic widgets and eight split ADC endpoints block the card's OF DAPM routes.

## Real initialization transfer
`p480_filter_links` transfers `msm_int_audrx_init` only when the WSA RX0 link containing the genuine donor callback was excluded. It assigns the callback to retained RX_CDC_DMA_RX_0, which uses bolero, not a renamed codec. Refuses a conflicting initializer or missing RX0. After direction filtering the lito card must have exactly one retained callback owner. Both link table and per-probe driver data are fresh allocations. No template mutation.
- One invocation per card lifetime; after a failed probe ASoC tears down that card's widgets and controls. A new allocation must be initialized anew, not incorrectly reuse a stale global flag. Removed obsolete `msm_aux_dev` global pointer (the Fix 2 AUX path now allocates per-card). Read actual bound AUX components via `card->aux_comp_list`.
- Retain TG's actual internal/common/TDM/MI2S/AUXPCM control creation and microphone widget creation. Check the DAPM widget creation return value.
- `PORT_MAP=sm_port_map|sm_port_map_v2|wcd937x` reflects TG branch selection by real bound AUX codec or live lito-v2 DT flag. Do not alter the tables.
- `CFLAGS_kona.o += -DCONFIG_SND_SOC_BOLERO=1` selects the genuine, already built A3b exports rather than dormant inline no-ops in the Kona donor header. CI verifies undefined external references in kona.o for bolero_set_port_map, bolero_info_create_codec_entry, bolero_register_wake_irq.
- WSA speaker mode and gain calls occur only inside the WSA-available branch; `WSA_INIT_SKIPPED reason=MACRO_NOT_PRESENT` when unavailable. No other direct wsa_macro_* calls occur in Kona. The real A3b bolero_set_port_map implementation (bolero-cdc.c:44-71) calls only registered macro callbacks, so the WSA map entry is ignored if its callback is absent. bolero_register_wake_irq (1052-1077) uses TX or VA, never WSA.
- Scope note: existing WCD938x AUX header config/stub selection is unchanged by Fix 3. The actual codec is registered in A3b; this patch does not enable additional headset-detection behavior or alter MBHC calibration policy.

## Exact ADC route exclusions
TG donor `micr0softstore/samsung_android_kernel_a52xq@6bf351bdf18bdb228db79e66f14a7a9c0178e5d7`, techpack/audio/asoc/codecs/wcd937x/wcd937x.c: ADC1_OUTPUT line 2193, ADC2_OUTPUT line 2194, ADC3_OUTPUT line 2287; routes use them at 2292, 2297, 2338 respectively. **ADC4_OUTPUT is not defined in this donor file**; its two references still belong to the exact unsupported shared-DT endpoint list reported on this WCD938x device. Do not assert it is a proven WCD937x widget. Actual A3b wcd938x.c line 3410 declares WCD_TX_OUTPUT and has no ADC1..4_OUTPUT widgets.
- Resolve registered AUX DAIs via their actual OF-backed descriptors and inspect real driver names. Also check retained link codecs for any WCD937x. Defer missing codec providers. Only apply ADC exclusions for WCD938x present and WCD937x absent.
- Match exactly ADC1_OUTPUT, ADC2_OUTPUT, ADC3_OUTPUT or ADC4_OUTPUT as sink or source; no pattern/prefix-wide suppression. Copy only retained OF route entries; leave original boot DTB untouched. Keep strict disable_route_checks=false.
- Exact prior live evidence CSV (ci/P480-A3C-DAPM-ROUTES-EXPECTED.csv): 27 pairs, 19 expected retained/8 expected omitted. Route reasons log with ROUTE_SKIPPED and all retained pairs with ROUTE_KEPT. The five Analog Mic routes remain mandatory. Normal errors from any other missing widget remain fatal.
- On actual successful card registration: ROUTES added=N failed=0, CARD_NUMBER, DEVICES pcm/compress, KCONTROLS count (ALSA controls_count). These logs are emitted only in the success path. The compiled strings are not evidence of an actual successful card registration.

## Safety and evidence
- The nonfatal A3c init branches in Fix 2 are unchanged: INIT_FAIL stage=platform|machine and return 0 from p472_a3a_start without tearing down rtac/adm/afe/asm/lsm. Actual card failure is reported separately.
- New A3C-only image MANGROVE-P480-A3C-HLT.boot.img; original P479 DTB (1c5a9a3e...) and ramdisk (54f78918...) pinned, HLT command line, no nohlt. Existing fallback artifact 14672521007 from 44809264636 is read-only, never rebuilt. CI pins its archive digest and inspects the unpacked fallback image for A3C markers.
- 48/42 with one Compress1 trim is the prior hardware-observed link outcome, and expected unchanged because link-direction filters are not changed. Complete post-fix as-built link and route manifests require the reviewed hardware boot. Use the read-only collector; it extracts all emitted kept/skipped/trim and route records. No fake widgets/codecs/DAIs, calibration writes, SELinux permissive changes, Thermal Guard changes, or DTB edits.

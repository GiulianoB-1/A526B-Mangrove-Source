# P480 A3C Fix 4: five directed invalid MIC BIAS routes

**NO FLASH until Claude reviews.** Base: hardware-tested Fix 3 source `26a6d4f4d9116f4015d0fbfda1f8661b66f86c47`.

## Reported prior hardware result

The genuine RX initializer executed on RX_CDC_DMA_RX_0, selected sm_port_map, and skipped unavailable WSA. All missing widget errors disappeared. Fix 3 kept 19 OF routes and skipped eight ADC pairs. The card returned -EINVAL at 50.1396 s because five non-supply Analog Mic widgets were connected to MIC BIAS supply sinks.

## Exact change

In the existing WCD938x-only filter, reject only these ordered (sink, source) pairs, preserving the prior eight ADC exceptions:

| Sink | Source |
|---|---|
| MIC BIAS1 | Analog Mic1 |
| MIC BIAS2 | Analog Mic2 |
| MIC BIAS3 | Analog Mic3 |
| MIC BIAS3 | Analog Mic4 |
| MIC BIAS4 | Analog Mic5 |

Log each with ROUTE_SKIPPED reason=SUPPLY_SINK_FROM_NON_SUPPLY. This is not an unrestricted cross-product or substring test. The five valid AMIC <- MIC BIAS pairs remain. No widget types or definitions, driver capabilities, DTB bytes, calibration behavior, Fix 3 init ownership, or strict route checking are changed. Expected 14 retained and 13 skipped, with all 27 live r02 pairs accounted for in ci/P480-A3C-DAPM-ROUTES-EXPECTED.csv. This matrix is a source-derived expectation, not new hardware evidence.

## Desk check at the Fix 3 baseline

- wcd938x.c:3313–3328 uses SND_SOC_DAPM_SUPPLY for MIC BIAS1..4.
- sound/soc/soc-dapm.c:2840 rejects supply sink/non-supply source with -EINVAL.
- sound/soc/soc-core.c:snd_soc_bind_card():1897 initializes all PCM runtimes before OF routes at 1925. Then card names are filled at 1933–1938, optional component metadata is added, optional late probe at 1953 (Kona has none), widgets finalized at 1957 and snd_card_register at 1959. After successful registration, core marks instantiated, updates DAPM and pinctrl sleep state. No guarantee of hardware success before testing.
- Instrument actual snd_card_register rc without changing return handling, scoped by CONFIG_P480_A3C, lito model and Kona OF compatible. On a successful machine registration log the real ALSA card number, id, shortname and driver (the fields in sound/core/init.c used by /proc/asound/cards), plus existing actual PCM/compress/control counts. No fabricated success.
- The A3a nonfatal failure branches and INIT_FAIL diagnostics are unchanged. No changes to Thermal Guard, SELinux, HLT, calibration policy, codec or DAI availability.

## Build and proof

Only A3C is rebuilt as MANGROVE-P480-A3C-HLT.boot.img. Existing NO-A3C fallback artifact 14672521007 remains read-only; verify its digest and marker-free decompressed kernel. Preserve pinned original P479 DTB and ramdisk/HLT hashes. Verify new registry and route-reason strings in the compiled A3C image and absence in the existing fallback and P479 kernel. No flash approval. Actual post-fix route and link records must be collected after separately approved hardware testing.

# P480 A3C hardware follow-up: MI2S and deferred-probe review

## Provenance and validation boundary

- Working branch: `feature/p480-a3c-native`; parent hardware-tested P480: `9572003672895f7c804299d4957759fd30859dcf`.
- Donor: `micr0softstore/samsung_android_kernel_a52xq@6bf351bdf18bdb228db79e66f14a7a9c0178e5d7`, `techpack/audio/asoc/`.
- A52 r02 pinned overlay source: `arch/arm64/boot/dts/samsung/a52/a52xq/a52xq_eur_open_w00_r02.dts`, Git blob SHA-1 `72de76f7bae4b29ccc8663013936c0667a954180`.
- This is a static source key-set audit, **not** a complete byte comparison of the running merged DT or a hardware validation. The r02 overlay does not contain every inherited base-DT node. The live r02 MI2S property/mask evidence below is from the supplied hardware report.

## DT property read audit across all 13 native A3c provider sources

Compared the literal property keys consumed by `of_property_*`, `of_parse_phandle`, `of_count_phandle_with_args` and `of_get_named_gpio` in the port against the exact TG donor file of the same name.

| File | Port keys | TG keys | Key-set difference |
| --- | ---: | ---: | --- |
| `msm-dai-q6-v2.c` | 31 | 30 | Port-only optional `qcom,msm-cdc-dma-data-align`; see below |
| `msm-pcm-q6-v2.c` | 3 | 3 | None |
| `msm-compress-q6-v2.c` | 1 | 1 | None |
| `msm-pcm-hostless.c` | 0 | 0 | None |
| `msm-pcm-afe-v2.c` | 0 | 0 | None |
| `msm-pcm-q6-noirq.c` | 2 | 2 | None |
| `msm-pcm-routing-v2.c` | 0 | 0 | None |
| `msm-pcm-routing-devdep.c` | 0 | 0 | None |
| `msm-dai-fe.c` | 0 | 0 | None |
| `msm-dai-stub-v2.c` | 0 | 0 | None |
| `msm-audio-effects-q6-v2.c` | 0 | 0 | None |
| `msm-qti-pp-config.c` | 0 | 0 | None |
| `kona.c` | 36 | 43 | TG-only CPS and FM properties; see below |

### Mismatch A: MI2S, hardware-blocking, corrected

- Old port read `qcom,msm-mi2s-lines`, absent on live A52 r02; all six probes returned `-EINVAL`.
- TG and supplied live r02 use **both** `qcom,msm-mi2s-rx-lines` and `qcom,msm-mi2s-tx-lines`. Supplied examples: primary RX=0x3/TX=0x0; secondary RX=0x1/TX=0x0.
- The port now requires both properties, validates both masks with `msm_dai_q6_mi2s_get_lineconfig`, accepts zero masks and applies the RX or TX mask based on the registered split DAI's stream direction. Its `struct msm_mi2s_pdata` is updated to hold both masks. Added `MI2S_CONFIG` diagnostic.
- **Residual donor shape discrepancy requiring Claude review:** TG defines one duplex MI2S DAI per interface and separate `rx_dai` / `tx_dai` state, while this SM7325-derived port has separate RX/TX DAI entries and a single per-entry state. Probe rc=0 alone does not establish that all 12 MI2S BE links can resolve or that audio playback works. Confirm runtime CPU DAI binding and line configuration for each retained BE.

### Mismatch B: optional CDC_DMA data-align key, requires confirmation

- `msm-dai-q6-v2.c` also reads `qcom,msm-cdc-dma-data-align`, which the exact TG `msm-dai-q6-v2.c` does not. The code tolerates the property being absent, leaving the zero-initialized alignment field.
- This is **not** a proven regression in the reported boot, since CDC_DMA providers probed rc=0. Check the merged live DT and the relevant runtime DMA alignment before any sound test; do not silently synthesize a value.

### Mismatch C: TG-only Kona consumer keys, explicitly not ported

The TG Kona machine reads seven additional properties absent in this reduced A3c Kona source:

- `qcom,cps_reg_phy_addr`
- `qcom,cps_threshold_levels`
- `qcom,cps_wsa_vbatt_temp_reg_addr`
- `qcom,cps_normal_values`
- `qcom,cps_lower1_values`
- `qcom,cps_lower2_values`
- `qcom,fm-lna-gpios`

These are missing **reads/features in the reduced provider**, not evidence that the keys are missing from the live DT. The WSA/physical amplifier and FM paths are intentionally excluded from the A3c diagnostic card. Do not port or enable the associated consumers without separate review.

### Kona deferred probe, corrected

- Each call to `populate_snd_card_dailinks` assembles a source table and makes a new **devm-owned card, DAI-link table and each CPU/codec/platform component array**. A shallow link copy is insufficient, because DT population sets static component `name` and `dai_name` fields to NULL.
- The filter now only compacts the probe-owned link array **before** OF component population. For platform validity, it accepts either permitted legacy name or an OF node resolved through permitted `asoc-platform-names` / `asoc-platform` phandle.
- `-EPROBE_DEFER` is preserved; the prior `codec_reg_done` conversion to `-EINVAL` is removed. The new `DEFER_MISSING` diagnostic identifies missing CPU/codec/platform components where detectable.
- The A3c platform or Kona-machine registration failure logs its failure but does not unwind A3a Q6 services.

## Review and acceptance gates

1. Source-only audits and single A3C-only Linux 5.10 compile pass. No NO-A3C or other variants in this build.
2. Repack from SHA256-pinned final P479 HLT boot, preserve DTB, ramdisk and cmdline exactly. Publish marked `DO-NOT-FLASH`.
3. **Claude review first** of source/boot provenance, and especially the residual duplex-vs-split MI2S DAI topology; **NO FLASH** without that review.
4. Only subsequently, on authorized hardware: six MI2S probe rc=0; `CARD register rc=0 name=lito-lagoonmtp-snd-card`; inspect retained RX/TX/VA CDC_DMA and MultiMedia/Compress links, `/proc/asound/cards` and deferral diagnostics. None of these on-device post-fix results has been observed yet.

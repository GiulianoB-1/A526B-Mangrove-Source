# P480 A3C CPU DAI direction and register-less routing review

**Safety: NO FLASH until Claude reviews.** Only the P480 A3C sound-card candidate changes. Existing NO-A3C fallback remains the unmodified artifact **14672521007** from successful run **44809264636** (artifact name `P480-NO-A3C-FINAL-P479-TEMPLATE-91e61c94908a9f6fe21b12564b60a4aa26878aca`). This workflow does not rebuild or replace it.

## Evidence

Reported hardware before this patch: TG A52 r02 WCD938x AUX and all providers register; first invalid PCM `Primary MI2S Capture` returns `-EINVAL` at 50.54 s; total `links=52 skipped=38`. Routing component reads flood `-EIO`, with one update_bits write failure. Kernel 5.10 `sound/soc/soc-pcm.c:soc_new_pcm()` requires a real CPU DAI to support each requested DPCM direction, whereas the 4.19 TG link table includes directions disabled by board DT.

## Implementation

- **Only on fresh per-probe devm card/link copies**, after static link filtering, CPU OF-node population, and optional QUAT/SENARY missing-DAI handling, call `p480_filter_cpu_stream_directions()`.
- For every `no_pcm` or `dynamic` link, find **each** CPU DAI via `snd_soc_find_dai_with_mutex()`. Missing required DAI returns `-EPROBE_DEFER`. Determine directional support with the kernel's `snd_soc_dai_stream_valid()`, not inferred data or faked `channels_min`.
- Clear unsupported `dpcm_playback` / `dpcm_capture`. If no direction remains, drop the link, increment skip count, and emit `P480 A3c LINK_SKIPPED name=... reason=CPU_DAI_NO_PLAYBACK|CPU_DAI_NO_CAPTURE|CPU_DAI_NO_STREAMS`. For a partially trimmed dual-direction link, retain it and emit `P480 A3c LINK_DIR_TRIM name=... cleared=playback|capture`.
- Emit `P480 A3c LINK_KEPT name=... playback=... capture=...` for **all** surviving links. These and `LINK_SKIPPED` markers, not the source-only expectations, form the authoritative runtime as-built manifest.
- The virtual `msm-pcm-routing` component now has register-less `.read` (return 0) and `.write` (no-op return 0) callbacks. TG software mixer get/put handlers remain unchanged. Writes emit rate-limited `ROUTING_REGLESS_WRITE reg=... val=...` to help investigate the prior update_bits call. As most controls use `SND_SOC_NOPM`, this does **not** identify a unique control or DAPM widget without a call trace.
- Set `disable_route_checks = false`. On successful registration only, emit `P480 A3c ROUTES added=%u failed=0` using `num_of_dapm_routes` (reported live DT: 27). On failure, do **not** claim all routes were added; the ASoC core logs missing sink/source widgets.
- On card success log actual `CARD_NUMBER` and `DEVICES pcm=... compress=...`. A3c provider/machine early init failures log `INIT_FAIL stage=platform|machine rc=...` and must not unwind A3a's rtac/adm/afe/asm/lsm.

## Source-predicted MI2S decisions, NOT yet hardware-verified

See `ci/P480-A3C-DIRECTION-EXPECTED.csv` for all 12 MI2S BE directions. With the **user-provided live r02 masks**:

| Expected drop | CPU direction unavailable | Runtime reason |
| --- | --- | --- |
| Primary MI2S Capture | prim TX=0x0 | `CPU_DAI_NO_CAPTURE` |
| Secondary MI2S Capture | sec TX=0x0 | `CPU_DAI_NO_CAPTURE` |
| Tertiary MI2S Playback | tert RX=0x0 | `CPU_DAI_NO_PLAYBACK` |
| Senary MI2S Playback | senary RX=0x0 | `CPU_DAI_NO_PLAYBACK` |

Quinary playback/capture remain `TAS_NOT_PORTED`. Quaternary RX/TX and Senary Capture stay **only if actual CPU DAIs resolve**. An initial estimate is `links=48 skipped=42` starting from the last **observed** `52/38`; CI **cannot establish these runtime counts**, and additional trims or missing DAIs may change them. The complete kept/skipped manifest must be collected from the next post-review hardware boot.

## Artifact contract

Only the new `MANGROVE-P480-A3C-HLT.boot.img` is built. The repacker pins the *original* P479 boot SHA, DTB SHA256 `1c5a9a3ec9e9391bedb4716996dc57e1a9c982e44f8c1c1196773fdbb364ec28`, ramdisk SHA256 `54f7891881a755536e908acd66fa2cf48bb24ed5f9f8d2fc4b0bf259f395a4c1`, and HLT cmdline (no `nohlt`). The new strings proof verifies changed A3C markers in the unpacked newly built image and their absence in the **original pinned P479** kernel. It does **not** falsely assert that the unchanged preexisting P480 NO-A3C fallback binary was rechecked in this build.

No `/efs`, ACDB, TAS25xx or other calibration writes, SELinux changes, dummy physical components, DAI capability fabrication, Thermal Guard changes, HLT overrides, DTB edits or P479 safety-gate relaxation.

# P484 review - NO FLASH
Parent: 20800fd1a12c429bade9144c0d27bf9e0d8b33a9 (successful P483; same Kona source as failed 845927bd0). No proof yet of the image physically flashed on device.

* A: Kona had CONFIG_SND_SOC_WCD938X unset locally, selecting the inline success stub. Select real A3b declarations for Kona, verify U/T symbols, and trace AUX binding, component init and one-time callback. The voice AUX appends to index 1, preserving the codec at index 0. Runtime component matching remains to be verified.
* B: SWR raw/pending/nested IRQ, wake arm and TLMM caller traces; regmap status before mask, WCD post-IRQ; root-only MBHC dump uses codec/master PM and real votes. Diagnostic reads are through regmap, not raw bypass. A failed PM get aborts the dump.
* C: TG fixed-width basic=16, ADM info=20, ADM topology=36; A3b donor CMA field made A3c basic=20, topology=40. Route all A3c calibration headers to canonical TG ABI and require compile-time assertions. ioctl compat translation itself is correct. msm_routing_set_cal -> msm_routing_load_topology -> msm_routing_get_topology. 0x10bcd=ASM_STREAM_CMD_CLOSE. No persistent calibration writes.
* D1: per CPU CDC DMA port gets a real core and audio clock manager reference before start, releases after stop/removal or on failure. Start payload and votes logged. TG relies on runtime PM through codec/AFE sequencing; do not rely on its timing in GKI.
* D2: all three ADM matrix callers accept nonnegative success, propagate negative errors; normalize PCM result.
* D3: best-effort afe_close on a failed start while refs held, preserve original error; log rollback result. ZERO ADC mapping override now falls back to DT like TG. SSR -EINVAL logs cause/reg/caller. HPH Digital Volume is present in donor under CONFIG_SND_SOC_IMPED_SENSING; enabling gain policy/DT changes deferred pending review, not a fabricated alias. Broader positive return audit and exact runtime gain/IRQ behavior remain unverified.

P479 DTB, ramdisk, cmdline and P482/P483 proof checks preserved; do not flash until Claude reviews. No TAS, speaker enablement, IPA, charging or SELinux changes.

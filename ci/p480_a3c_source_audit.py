#!/usr/bin/env python3
"""P480 A3c read-only source contract audit.

Checks that direct-source audio integration still uses real built-in
Q6/ASoC providers, maintains explicit fail-closed ABI boundaries,
and retains all NO-FLASH/P479 policy gates.

This performs file reads only. It is not a hardware pass.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


def check(test: bool, message: str) -> None:
    if not test:
        raise SystemExit("P480 A3c safety FAIL: " + message)
    print("P480 A3c safety PASS: " + message, flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()

    def read(path: str) -> str:
        target = root / path
        check(target.is_file(), "required committed source: " + path)
        return target.read_text(encoding="utf-8")

    config = read("ci/build.config")
    if "CONFIG_P480_A3C=y" not in config.splitlines():
        check("# CONFIG_P480_A3C is not set" in config or
              "CONFIG_P480_A3C=" not in config,
              "A3c disabled in this source tree")
        print("P480 A3c audit: disabled baseline, no A3c assertion needed")
        return

    for flag in ("CONFIG_P472_A3A_Q6=y", "CONFIG_P472_A3B_B3=y",
                 "CONFIG_P479_H4=y", "CONFIG_SECURITY_SELINUX=y"):
        check(flag in config.splitlines(), "pinned prerequisite " + flag)
    manifest = json.loads(read("ci/build-manifest.json"))
    check(manifest.get("strict") is True, "strict source manifest")
    check(manifest.get("flash_authorized") is False,
          "NO FLASH hard policy")
    check(manifest.get("missing_required") == [],
          "no missing required source")
    check(manifest.get("stage") == "P480-A3c-native-candidate",
          "P480 stage still diagnostic")
    check(manifest.get("p480_source_materialized") is True,
          "native source materialized, not generator chain")
    check(hashlib.sha256(config.encode()).hexdigest() ==
          manifest.get("kernel_config_sha256"),
          "pinned source configuration SHA256")

    kconfig = read("drivers/soc/qcom/Kconfig")
    section = re.search(r"(?m)^config P480_A3C\s*\n(?P<body>[\s\S]*?)(?=^config |\Z)",
                        kconfig)
    check(bool(section), "Kconfig entry present")
    check(re.search(r"(?m)^\s+default n\s*$", section["body"]) is not None,
          "A3c Kconfig is off by default")
    check("depends on P472_A3B_B3 && P472_A3A_Q6" in section["body"],
          "A3a/A3b dependencies preserved")

    audio_make = read("drivers/soc/qcom/p472_tg_audio/Makefile")
    card_make = read("drivers/soc/qcom/p472_tg_audio/a3c/Makefile")
    codec_make = read("drivers/soc/qcom/p472_tg_audio/a3c/codecs/Makefile")
    check("obj-$(CONFIG_P480_A3C) += a3c/" in audio_make,
          "A3c integrated as native Kbuild directory")

    providers = (
        "msm-pcm-q6-v2", "msm-compress-q6-v2",
        "msm-pcm-hostless", "msm-pcm-afe-v2",
        "msm-pcm-q6-noirq", "msm-pcm-routing-v2",
        "msm-pcm-routing-devdep", "msm-dai-q6-v2",
        "msm-dai-fe", "msm-dai-stub-v2",
        "msm-audio-effects-q6-v2", "msm-qti-pp-config",
        "platform_init", "kona",
    )
    for name in providers:
        check(name + ".o" in card_make, "native provider built-in: " + name)
        read("drivers/soc/qcom/p472_tg_audio/a3c/" + name + ".c")
    check("obj-$(CONFIG_P480_A3C) += msm_stub.o" in codec_make,
          "MSM proxy dummy codec explicitly scoped to A3c")
    stub = read("drivers/soc/qcom/p472_tg_audio/a3c/codecs/msm_stub.c")
    check('qcom,msm-stub-codec' in stub and
          "snd_soc_register_component" in stub and
          "platform_driver_register" in stub,
          "generic proxy codec is real registered component, not FE/BE DSP substitute")
    check("q6asm_" not in stub and "q6afe_" not in stub,
          "proxy codec does not impersonate Q6 DSP service")

    asm = read("drivers/soc/qcom/p472_tg_audio/dsp/q6asm.c")
    afe = read("drivers/soc/qcom/p472_tg_audio/dsp/q6afe.c")
    for symbol in ("q6asm_open_write_with_retry", "q6asm_open_read_with_retry",
                   "q6asm_cpu_buf_release_nolock"):
        check(re.search(r"(?m)^(?:int|void)\s+" + symbol + r"\s*\(", asm) is not None,
              "real ASM function implemented: " + symbol)
        check("EXPORT_SYMBOL(" + symbol + ")" in asm,
              "ASM function exported: " + symbol)
    for symbol in ("afe_q6_update_enc_format", "afe_q6_update_sbm",
                   "afe_set_lc3_channel_mode", "afe_send_cdc_dma_data_align"):
        check(re.search(r"(?m)^int\s+" + symbol + r"\s*\(", afe) is not None,
              "real AFE function implemented: " + symbol)
        check("EXPORT_SYMBOL(" + symbol + ")" in afe,
              "AFE function exported: " + symbol)

    # The legacy ADM matrix mapper rejects a nonzero FE ID because the
    # newer 5.4 calibration lookup is not yet safely ported. Never discard
    # this error and proceed with PCM/compressed PP configuration as though
    # routing had succeeded (the original P480 draft did exactly that).
    routing = read("drivers/soc/qcom/p472_tg_audio/a3c/msm-pcm-routing-v2.c")
    check(routing.count("= adm_matrix_map(") == 3,
          "all three ADM matrix-map return values explicitly checked")
    check(not re.search(r"(?m)^\s*adm_matrix_map\(", routing),
          "no unchecked raw ADM matrix-map calls")
    for word in ("P480 A3c ADM build_matrix", "P480 A3c ADM compressed",
                 "P480 A3c ADM PCM"):
        check(word in routing, "matrix routing error is logged: " + word)
    check("return p480_map_rc;" in routing and
          "mutex_unlock(&routing_lock);" in routing,
          "unsupported compressed matrix routing propagates failure")
    check("return ret;" in routing and
          "routing not applied" in routing,
          "unsupported PCM matrix routing propagates failure")

    shims = (
        ("p480_adm_compat.h", "p480_native_adm_open", "adm_open",
         "msm-pcm-routing-v2.h"),
        ("p480_q6asm_compat.h", "p480_native_pcm_format_v5",
         "q6asm_enc_cfg_blk_pcm_format_support_v5", "msm-pcm-q6-v2.c"),
        ("p480_afe_proxy_compat.h", "p480_native_rt_proxy_port_read",
         "afe_rt_proxy_port_read", "msm-pcm-afe-v2.c"),
    )
    for filename, wrapper, legacy, user in shims:
        data = read("drivers/soc/qcom/p472_tg_audio/a3c/" + filename)
        caller = read("drivers/soc/qcom/p472_tg_audio/a3c/" + user)
        check(wrapper in data and "-EOPNOTSUPP" in data,
              "unsupported " + legacy + " ABI rejected, never faked")
        check("__asm__(\"" + legacy + "\")" in data,
              "legacy " + legacy + " resolves to real linker symbol")
        check('#include "' + filename + '"' in caller,
              "A3c caller selects guarded " + legacy + " ABI")
    compat = read("drivers/soc/qcom/p472_tg_audio/a3c/p480_q6asm_compat.h")
    check("p480_native_render_mode" in compat and
          "return p480_tg_render_mode(ac, render_mode, 0);" in compat,
          "render mode explicitly preserves original RX direction")

    # P480: A3c platform and Kona machine must never be device-initcalls.
    platform_init = read("drivers/soc/qcom/p472_tg_audio/a3c/platform_init.c")
    kona_source = read("drivers/soc/qcom/p472_tg_audio/a3c/kona.c")
    a3a_init = read("drivers/soc/qcom/p472_tg_audio/dsp/p472_a3a_q6_init.c")
    cal_source = read("drivers/soc/qcom/p472_tg_audio/dsp/audio_calibration.c")
    check("module_init(p480_audio_platform_init)" not in platform_init and
          "module_platform_driver(kona_asoc_machine_driver)" not in kona_source,
          "A3c platform and Kona machine have no premature device initcall")
    check("rc = p480_audio_platform_init();" in a3a_init and
          "rc = p480_kona_machine_init();" in a3a_init and
          a3a_init.index("rc = q6lsm_init();") <
          a3a_init.index("rc = p480_audio_platform_init();") <
          a3a_init.index("rc = p480_kona_machine_init();"),
          "A2a then A3a Q6 then A3c platform then machine")
    check("CAL_REGISTER_SKIPPED reason=not_initialized" in cal_source and
          "if (!audio_cal_is_initialized())" in cal_source and
          "P480 A3c PLATFORM_SKIPPED reason=a2a_not_ready" in a3a_init and
          "P480 A3c PLATFORM_SKIPPED reason=a3a_services_failed" in a3a_init,
          "calibration and Q6 fail-closed ordering gates")

    # P480 A3c hardware follow-up: TG has independent RX/TX SD lines.
    mi2s_source = read("drivers/soc/qcom/p472_tg_audio/a3c/msm-dai-q6-v2.c")
    mi2s_header = read("drivers/soc/qcom/p472_tg_audio/a3c/msm-dai-q6-v2.h")
    check('"qcom,msm-mi2s-rx-lines"' in mi2s_source and
          '"qcom,msm-mi2s-tx-lines"' in mi2s_source and
          '"qcom,msm-mi2s-lines"' not in mi2s_source and
          "msm_dai_q6_mi2s_get_lineconfig(mi2s_pdata->rx_sd_lines" in mi2s_source and
          "msm_dai_q6_mi2s_get_lineconfig(mi2s_pdata->tx_sd_lines" in mi2s_source and
          "u16 rx_sd_lines;" in mi2s_header and
          "u16 tx_sd_lines;" in mi2s_header,
          "A52 TG MI2S RX/TX DT properties and direction-specific line config")
    check("p480_copy_card_links" in kona_source and
          kona_source.count("devm_kmemdup(") >= 5 and
          "p480_platform_is_ported" in kona_source and
          "p480_log_deferred_components(card);" in kona_source and
          "if (codec_reg_done)" not in kona_source and
          kona_source.index("p480_filter_links(card);") <
          kona_source.index("msm_populate_dai_link_component_of_node(card);") <
          kona_source.index("devm_snd_soc_register_card(&pdev->dev, card);"),
          "Kona fresh card/link/CPU/codec/platform arrays on retry; filter before DT resolution")
    check("a3c_nonfatal:" in a3a_init and
          a3a_init.count("goto a3c_nonfatal;") == 2 and
          "goto undo_lsm;" not in a3a_init and
          "schedule_delayed_work(&p472_a3a_version_job" in a3a_init,
          "A3c registration failure preserves A3a Q6 services")

    # TG A52 duplex MI2S and AUX WCD938x must not regress to SM7325.
    dailink = read("drivers/soc/qcom/p472_tg_audio/a3c/msm_dailink.h")
    mi2s_names = ("Primary", "Secondary", "Tertiary", "Quaternary",
                  "Quinary", "Senary")
    for name in mi2s_names:
        check(('.name = "' + name + ' MI2S"') in mi2s_source,
              "TG duplex MI2S DAI: " + name)
    for idx, prefix in enumerate(("pri", "sec", "tert", "quat", "quin", "sen")):
        for direction in ("rx", "tx"):
            start = dailink.index("SND_SOC_DAILINK_DEFS(" +
                                  prefix + "_mi2s_" + direction + ",")
            chunk = dailink[start:start + 240]
            check('COMP_CPU("msm-dai-q6-mi2s.' + str(idx) + '")' in chunk,
                  "TG MI2S CPU ID " + prefix + "/" + direction)
    for link in ("rx_dma_rx0", "rx_dma_rx1", "rx_dma_rx2",
                 "rx_dma_rx3", "tx_dma_tx3", "tx_dma_tx4",
                 "va_dma_tx0", "va_dma_tx1", "va_dma_tx2"):
        start = dailink.index("SND_SOC_DAILINK_DEFS(" + link + ",")
        end = dailink.index(");", start)
        chunk = dailink[start:end]
        check('COMP_CODEC("bolero_codec", "' in chunk and
              "wcd938x_codec" not in chunk and
              "wcd938x_cdc" not in chunk,
              "A52 TG bolero-only CDC_DMA link " + link)
    check("aux[i].dlc.of_node = node;" in kona_source and
          '"qcom,codec-aux-devs"' in kona_source and
          "aux[i].init = msm_aux_codec_init;" in kona_source and
          "p480_filter_optional_mi2s(card);" in kona_source and
          "MI2S_CPU_OF link=" in kona_source and
          "TAS_NOT_PORTED" in kona_source and
          "MI2S_CPU_DAI_UNRESOLVED" in kona_source,
          "WCD938x AUX path, true OF CPU matching and safe optional MI2S")
    check("struct msm_dai_q6_mi2s_dai_config tx_dai;" in mi2s_source and
          "struct msm_dai_q6_mi2s_dai_config rx_dai;" in mi2s_source and
          "MI2S_DAI_REGISTER device=" in mi2s_source and
          "MSM_PRIM_MI2S_RX" not in mi2s_header,
          "TG duplex RX/TX DAI state and diagnostics")

    # P480 reviewer boots are packaged only by the native build workflow.
    read("ci/p480_boot_repack.py")
    read("ci/p480_p479_boot_repack.py")
    read("ci/p480_collect_audio_readonly.sh")
    read("ci/P479-boot-provenance.json")
    read("ci/P479-boot-dtb.sha256")
    package = read(".github/workflows/direct-source-build.yml")
    check("inputs.p480_twins" in package and
          "inputs.p480_compare" in package and
          "if: ${{ (github.event_name == 'workflow_dispatch' && inputs.p480_twins && !inputs.p480_compare) || (github.event_name == 'push' && contains(github.event.head_commit.message, '[P480-REPACK-CORRECT-VERMAGIC]')) }}" in package and
          "if: ${{ (github.event_name == 'workflow_dispatch' && (inputs.p480_twins || inputs.p480_compare))" in package and
          "P480-A3C-FINAL-P479-TEMPLATE-" in package and
          "P480-NO-A3C-FINAL-P479-TEMPLATE-" in package,
          "twin images only manual or one-shot pinned-vermagic repack; no comparison boot")
    check("LOCALVERSION: ''" in package and
          "strings','-a" in package and
          "P480-vermagic-gate.json" in package,
          "native P480 boot twins require exact original P479 module vermagic")
    check("bash ci/p480_symbol_comparison.sh" in package and
          "P479-P480-FOUR-SOURCE-SYMBOL-EVIDENCE-" in package and
          "if: ${{ (github.event_name == 'workflow_dispatch' && inputs.p480_twins && !inputs.p480_compare) || (github.event_name == 'push' && contains(github.event.head_commit.message, '[P480-REPACK-CORRECT-VERMAGIC]')) }}" in package,
          "comparison runs only evidence; never boot packaging")
    check("p479-equivalence-reference" in package and
          "p479-reference-14131467025.zip" in package and
          "ci/P479-boot-dtb.sha256" in package and
          "ci/P479-boot-provenance.json" in package,
          "exact P479 final HLT boot and DTB pins are mandatory")
    check("phase446wx-source-backup" not in package and
          "boot-f0bisect-gki-p6-v1.img" not in package and
          "ci/p480_p479_boot_repack.py" in package and
          "--control" in package and
          "repacked_DTB_sha256" in package and
          "repacked_ramdisk_sha256" in package and
          "repacked_cmdline" in package and
          "sha256sum -c SHA256SUMS" in package,
          "template preservation and control checks are fail-closed")
    check("P480-P479-BOOT-CONTROL-" in package and
          "A3C_only_config_difference" in package and
          "source_control_is_55bab_repair_not_original_P479" in package,
          "P479 control and changed PIL/SSR ABI are reported")
    print("P480 A3c source-only integration contracts PASS; no hardware validation")


if __name__ == "__main__":
    main()

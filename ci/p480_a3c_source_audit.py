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
    check(not __import__("re").search(r"(?m)^\\s*adm_matrix_map\\(", routing),
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

    read("ci/p480_boot_repack.py")
    read("ci/p480_hlt_peer.py")
    read("ci/p480_collect_audio_readonly.sh")
    package = read(".github/workflows/direct-source-build.yml")
    check("P480-A3c-HLT-UNVERIFIED-DO-NOT-FLASH.img" in package and
          "sha256sum -c SHA256SUMS" in package,
          "diagnostic packaging and SHA256 gate retained")
    print("P480 A3c source-only integration contracts PASS; no hardware validation")


if __name__ == "__main__":
    main()

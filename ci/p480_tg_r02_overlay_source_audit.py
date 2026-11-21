#!/usr/bin/env python3
"""Audit pinned Samsung A52XQ r02 DTBO *source*; never claim runtime merge.

This verification is deliberately separate from the Phase446WX boot DTB
preflight. Samsung's device-specific DTBO can supply properties not present
in the boot image's generic Lagoon DTB. Source existence does not prove
that the DTBO was installed, selected, or merged by the bootloader.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

DONOR_REPO = "micr0softstore/samsung_android_kernel_a52xq"
DONOR_COMMIT = "6bf351bdf18bdb228db79e66f14a7a9c0178e5d7"
DONOR_PATH = "arch/arm64/boot/dts/samsung/a52/a52xq/a52xq_eur_open_w00_r02.dts"
DONOR_BLOB_SHA1 = "72de76f7bae4b29ccc8663013936c0667a954180"
CARD_MODEL = "lito-lagoonmtp-snd-card"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit("P480 pinned TG r02 source FAIL: " + message)


def git_blob_sha1(data: bytes) -> str:
    # Git hashes exact bytes with a leading "blob <length>\0" object header.
    prefix = b"blob " + str(len(data)).encode("ascii") + b"\x00"
    return hashlib.sha1(prefix + data).hexdigest()


def fragment(text: str, number: int) -> str:
    # The decompiled Samsung DTBO keeps each top-level fragment on one-tab lines.
    matches = list(re.finditer(r"(?m)^\tfragment@" + str(number) + r"\s*\{", text))
    require(len(matches) == 1, f"expected exactly one fragment@{number}")
    begin = matches[0].start()
    following = re.search(r"(?m)^\tfragment@\w+\s*\{", text[matches[0].end():])
    require(following is not None, f"no fragment follows fragment@{number}")
    end = matches[0].end() + following.start()
    return text[begin:end]


def prop(block: str, name: str) -> str:
    expression = r"(?ms)^\s*" + re.escape(name) + r"\s*=\s*(.*?);"
    matches = re.findall(expression, block)
    require(len(matches) == 1, f"expected one {name} property")
    return matches[0]


def string_values(value: str) -> list[str]:
    return re.findall(r'"([^"]+)"', value)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-dts", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()

    raw = args.reference_dts.read_bytes()
    require(git_blob_sha1(raw) == DONOR_BLOB_SHA1,
            "untrusted donor source bytes; Git blob SHA1 differs")
    text = raw.decode("utf-8")
    require('model = "Samsung A52XQ PROJECT (board-id,02)";' in text,
            "not A52XQ board revision 02")
    require('qcom,board-id = <0x8 0x2>;' in text,
            "board-id mismatch")

    f2 = fragment(text, 2)
    f126 = fragment(text, 126)
    require(prop(f2, "qcom,model").strip() == f'"{CARD_MODEL}"',
            "TG model does not match P480 card contract")
    routes_2 = string_values(prop(f2, "qcom,audio-routing"))
    routes_126 = string_values(prop(f126, "qcom,audio-routing"))
    require(len(routes_2) == 170 and len(routes_126) == 54,
            "TG r02 audio route string counts changed")
    require(len(routes_2) % 2 == 0 and len(routes_126) % 2 == 0,
            "audio routing must use destination/source pairs")
    require(routes_2 != routes_126, "fragment@126 must override initial routing")
    require(routes_126[:6] == [
        "AMIC1", "MIC BIAS1", "MIC BIAS1", "Analog Mic1", "AMIC2", "MIC BIAS2"
    ], "r02 final routing prefix changed")
    require(routes_126[-4:] == [
        "VA SWR_ADC2", "ADC3_OUTPUT", "VA SWR_ADC3", "ADC4_OUTPUT"
    ], "r02 final routing suffix changed")

    # Samsung's decompiled DTBO uses unresolved target phandles plus fixups.
    # This checks source references, not actual runtime application.
    require('target = <0xffffffff>;' in f2 and
            'target = <0xffffffff>;' in f126,
            "expected DTBO fixup targets missing")
    fixup = prop(text[text.index("\t__fixups__ {"):], "lagoon_snd")
    require('"/fragment@2:target:0"' in fixup and
            '"/fragment@126:target:0"' in fixup,
            "Samsung DTBO lagoon_snd fixups changed")
    platforms = string_values(prop(f126, "asoc-platform-names"))
    require(len(platforms) == 14 and platforms[-1] == "q6audio-adaptation",
            "r02 final audio platform override changed")

    report = {
        "schema": "P480-pinned-TG-A52XQ-r02-source-audio-overlay-v1",
        "status": "TG_R02_AUDIO_OVERLAY_SOURCE_PRESENT_UNMERGED",
        "donor_repository": DONOR_REPO,
        "donor_commit": DONOR_COMMIT,
        "donor_path": DONOR_PATH,
        "donor_blob_sha1": DONOR_BLOB_SHA1,
        "donor_source_url": "https://github.com/" + DONOR_REPO +
            "/blob/" + DONOR_COMMIT + "/" + DONOR_PATH,
        "board_revision": "02",
        "card_model_from_fragment_2": CARD_MODEL,
        "initial_audio_routing_string_count": len(routes_2),
        "final_audio_routing_string_count": len(routes_126),
        "final_routing_from_fragment": 126,
        "final_audio_routing_pairs": list(zip(routes_126[0::2], routes_126[1::2])),
        "final_platform_names": platforms,
        "lagoon_snd_fixups_present": True,
        "boot_template_dtb_modified": False,
        "actual_device_dtbo_verified": False,
        "bootloader_applied_dtbo_verified": False,
        "merged_live_fdt_verified": False,
        "a3c_binding_verified": False,
        "hardware_validated": False,
        "flash_authorized": False,
        "flashable_candidate": False,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
    print("P480 TG pinned r02 SOURCE PASS: model=" + CARD_MODEL +
          ", initial routing=170, final routing=54, overlay targets lagoon_snd")
    print("NOT proof of deployed DTBO or merged FDT; NO BOOT IMAGE; NO FLASH")


if __name__ == "__main__":
    main()

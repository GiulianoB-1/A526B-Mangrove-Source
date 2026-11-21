#!/usr/bin/env python3
"""P480: replace ONLY the kernel in the exact final P479 HLT boot (NO FLASH).

Every input is pinned in ci/P479-boot-provenance.json. This deliberately
cannot read or fall back to any Phase447C/WX intermediate boot image.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import struct
from pathlib import Path

from p480_boot_repack import align, calculate_boot_id, parse_boot


def digest(data: bytes) -> str:
    """SHA256 digest of exact bytes."""
    return hashlib.sha256(data).hexdigest()


def require(ok: bool, why: str) -> None:
    if not ok:
        raise SystemExit("P480 P479-template repack BLOCKED: " + why)


def original_metadata(source: bytes, meta: dict, dtb_pin: str) -> dict:
    require(len(source) == meta["original_boot_size"], "P479 partition length drift")
    require(digest(source) == meta["original_boot_sha256"], "not the original P479 HLT boot")
    p = parse_boot(source)
    require(p["header_version"] == meta["header_version"] == 2, "boot header version drift")
    require(p["page_size"] == meta["page_size"] == 4096, "boot page size drift")
    require(p["dtb"] and len(p["dtb"]) == meta["original_boot_dtb_size"],
            "missing or changed final P479 DTB")
    require(digest(p["dtb"]) == meta["original_boot_dtb_sha256"] == dtb_pin,
            "final P479 boot DTB SHA256 mismatch")
    require(digest(p["ramdisk"]) == meta["original_ramdisk_sha256"],
            "P479 ramdisk SHA256 mismatch")
    require(digest(p["kernel"]) == meta["original_gzip_kernel_sha256"],
            "P479 gzip kernel hash mismatch")
    require(digest(gzip.decompress(p["kernel"])) ==
            meta["original_uncompressed_kernel_sha256"],
            "P479 uncompressed kernel mismatch")
    require(p["cmdline"] == meta["cmdline"], "P479 command line drift")
    require(not any(word == "nohlt" or word.startswith("nohlt=")
                    for word in p["cmdline"].split()),
            "nohlt must be absent in original final P479 HLT boot")
    require(p["recovery_dtbo_size"] == 0,
            "recovery DTBO needs header relocation; fail closed")
    require(p["dtb_size"] > 0, "missing boot DTB")
    # Refuse to erase any unexpected data/signature outside boot components.
    last = align(p["end_offset"], p["page_size"])
    require(last <= len(source) and not any(source[last:]),
            "nonzero partition trailer or unknown AVB signature")
    return p


def image_difference(a: bytes, b: bytes) -> dict:
    """Report raw Image differences, without assuming they are build stamps."""
    if a == b:
        return {"exact_raw_image_match": True, "original_bytes": len(a),
                "rebuilt_bytes": len(b), "different_bytes": 0,
                "first_different_offset": None,
                "only_build_stamp_differences_proven": True}
    first = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y),
                 min(len(a), len(b)))
    count = sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))
    return {"exact_raw_image_match": False, "original_bytes": len(a),
            "rebuilt_bytes": len(b), "different_bytes": count,
            "first_different_offset": first,
            "only_build_stamp_differences_proven": False,
            "unclassified_difference_requires_review": True}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, required=True)
    ap.add_argument("--kernel", type=Path, required=True,
                    help="gzip -n compressed native Image from this build")
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--report", type=Path, required=True)
    ap.add_argument("--variant", required=True)
    ap.add_argument("--native-image", type=Path, required=True)
    ap.add_argument("--control", action="store_true",
                    help="compare a separately rebuilt P479-equivalent Image")
    args = ap.parse_args()
    meta = json.loads(Path("ci/P479-boot-provenance.json").read_text())
    raw_pin = Path("ci/P479-boot-dtb.sha256").read_text().strip().split()
    require(len(raw_pin) == 2 and raw_pin[1] == "P479-boot-final.dtb",
            "invalid immutable DTB SHA256 pin file")
    require(meta["flash_authorized"] is False, "source metadata permits flash")
    source = args.source.read_bytes()
    original = original_metadata(source, meta, raw_pin[0])
    compressed = args.kernel.read_bytes()
    require(compressed.startswith(b"\x1f\x8b"), "new kernel must be Image.gz")
    raw_image = gzip.decompress(compressed)
    require(raw_image == args.native_image.read_bytes(),
            "compressed kernel differs from the exact compiled native Image")
    page = original["page_size"]
    header = bytearray(original["header_page"])
    require(len(compressed) <= 0xffffffff, "kernel length out of bounds")
    struct.pack_into("<I", header, 8, len(compressed))
    new_boot_id = calculate_boot_id([
        (compressed, len(compressed)),
        (original["ramdisk"], original["ramdisk_size"]),
        (original["second"], original["second_size"]),
        (original["recovery_dtbo"], original["recovery_dtbo_size"]),
        (original["dtb"], original["dtb_size"]),
    ])
    header[576:608] = new_boot_id
    result = bytearray(header)
    for payload in (compressed, original["ramdisk"], original["second"],
                    original["recovery_dtbo"], original["dtb"]):
        result.extend(payload)
        result.extend(b"\x00" * (align(len(result), page) - len(result)))
    require(len(result) <= len(source), "new boot sections exceed partition")
    result.extend(b"\x00" * (len(source) - len(result)))
    rebuilt = parse_boot(bytes(result))
    kept = {
        "partition_size": len(result) == len(source),
        "header_version": rebuilt["header_version"] == original["header_version"],
        "page_size": rebuilt["page_size"] == original["page_size"],
        "board": rebuilt["board"] == original["board"],
        "os_version": rebuilt["os_version"] == original["os_version"],
        "kernel_address": rebuilt["kernel_addr"] == original["kernel_addr"],
        "ramdisk_address": rebuilt["ramdisk_addr"] == original["ramdisk_addr"],
        "second_address": rebuilt["second_addr"] == original["second_addr"],
        "tags_address": rebuilt["tags_addr"] == original["tags_addr"],
        "dtb_address": rebuilt["dtb_addr"] == original["dtb_addr"],
        "header_size": rebuilt["header_size"] == original["header_size"],
        "header_except_kernel_size_and_boot_id":
            (rebuilt["header_page"][:8] == original["header_page"][:8] and
             rebuilt["header_page"][12:576] == original["header_page"][12:576] and
             rebuilt["header_page"][608:] == original["header_page"][608:]),
        "exact_cmdline": rebuilt["cmdline"] == original["cmdline"] == meta["cmdline"],
        "nohlt_absent": "nohlt" not in rebuilt["cmdline"].split() and
            not any(w.startswith("nohlt=") for w in rebuilt["cmdline"].split()),
        "exact_ramdisk": rebuilt["ramdisk"] == original["ramdisk"],
        "exact_DTB": rebuilt["dtb"] == original["dtb"],
        "exact_second": rebuilt["second"] == original["second"],
        "exact_recovery_dtbo": rebuilt["recovery_dtbo"] == original["recovery_dtbo"],
        "new_gzip_kernel": rebuilt["kernel"] == compressed,
        "boot_id_recalculated": rebuilt["id"] == new_boot_id,
        "original_P479_DTB_SHA256": digest(rebuilt["dtb"]) ==
            meta["original_boot_dtb_sha256"],
    }
    require(all(kept.values()),
            "non-kernel payload/header invariants failed: " +
            ", ".join(k for k, v in kept.items() if not v))
    # The output is never flash-approved. Internal boot-ID/size header
    # bookkeeping is the only permitted non-payload modification.
    report = {
        "status": "P479_FINAL_HLT_TEMPLATE_KERNEL_ONLY_REPACK_PASS",
        "variant": args.variant,
        "original_P479_boot_sha256": meta["original_boot_sha256"],
        "repacked_boot_sha256": digest(bytes(result)),
        "original_P479_DTB_sha256": digest(original["dtb"]),
        "repacked_DTB_sha256": digest(rebuilt["dtb"]),
        "original_P479_ramdisk_sha256": digest(original["ramdisk"]),
        "repacked_ramdisk_sha256": digest(rebuilt["ramdisk"]),
        "original_P479_cmdline": original["cmdline"],
        "repacked_cmdline": rebuilt["cmdline"],
        "original_P479_Image_sha256":
            meta["original_uncompressed_kernel_sha256"],
        "rebuilt_Image_sha256": digest(raw_image),
        "gzip_image_sha256": digest(compressed),
        "invariants": kept,
        "changed_header_fields": ["kernel_size", "boot_id"],
        "original_P479_artifact": meta["source_artifact_id"],
        "flash_authorized": False,
        "hardware_audio_verified": False,
    }
    if args.control:
        old_raw = gzip.decompress(original["kernel"])
        report["control_kernel_diff"] = image_difference(old_raw, raw_image)
        report["control_original_image_sha256"] = digest(old_raw)
        report["control_rebuilt_image_sha256"] = digest(raw_image)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result)
    args.report.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
    print("P480 final-P479 HLT template repack PASS, variant", args.variant)
    print("P479 DTB SHA256", report["original_P479_DTB_sha256"])
    print("Original and repacked cmdline:", report["repacked_cmdline"])
    if args.control:
        print("P479 control Image comparison:", json.dumps(report["control_kernel_diff"]))
    print("NO FLASH. Source HLT DTB/ramdisk/cmdline byte-identical.")

if __name__ == "__main__":
    main()

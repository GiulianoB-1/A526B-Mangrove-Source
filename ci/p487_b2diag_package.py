#!/usr/bin/env python3
"""Only append the two reviewed B2DIAG boot-header arguments. NO FLASH."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
from p480_boot_repack import parse_boot
from p487_effective_cmdline import effective_cmdline

EXTRA = b" a52.diag=1 a52.mss_ssr_test=1"
FIELDS = ((64, 576), (608, 1632))

def sha(b):
    return hashlib.sha256(b).hexdigest()

def require(ok, why):
    if not ok:
        raise SystemExit("P487 B2DIAG BLOCKED: " + why)

def append_exact(source, config):
    before = parse_boot(source)
    result, effective, removed = effective_cmdline(source, config, EXTRA.decode().split())
    after = parse_boot(result)
    for key in before:
        if key not in ("cmdline", "header_page"):
            require(before[key] == after[key], "changed parsed field " + key)
    return result, before, after, effective, removed

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--package", type=Path, required=True)
    ap.add_argument("--native-image", type=Path, required=True)
    ap.add_argument("--commit", required=True)
    args = ap.parse_args()
    pkg = args.package
    staged = pkg / "MANGROVE-P487-STAGED-A3C-HLT.boot.img"
    source = staged.read_bytes()
    meta = json.loads(Path("ci/P479-boot-provenance.json").read_text())
    proof = json.loads((pkg / "P480-A3C-init-order-repack.json").read_text())
    require(all(proof["invariants"].values()) and not proof["flash_authorized"], "prior repack audit")
    require(sha(source) == proof["repacked_boot_sha256"], "staged boot hash")
    p = parse_boot(source)
    require(p["cmdline"] == meta["cmdline"] == proof["repacked_cmdline"], "staged cmdline pin")
    require(sha(p["dtb"]) == meta["original_boot_dtb_sha256"], "DTB pin")
    require(sha(p["ramdisk"]) == meta["original_ramdisk_sha256"], "ramdisk pin")
    native = args.native_image.read_bytes()
    require(gzip.decompress(p["kernel"]) == native, "compiled kernel mismatch")
    for marker in (b"P487 NOTIF reg name=", b"P487 NOTIF unreg name=",
                   b"P487 NOTIF begin name=", b"P487 NOTIF call name=",
                   b"P487 MSS SSR_TEST", b"a52.mss_ssr_test=",
                   b"P470 MSS NOTIF event=", b"P487 INIT_MODEM"):
        require(marker in native, "missing linked marker " + repr(marker))
    # early_param stores the bare parameter name; __setup stores its
    # explicitly supplied "=" suffix. Verify exact linked NUL-terminated
    # names and their actual registrations, not a loose substring.
    gates = Path("drivers/a52_447/a52_447_gates.c").read_text()
    sysfs = Path("drivers/remoteproc/remoteproc_sysfs.c").read_text()
    require('early_param("a52.diag", a52_diag_parse);' in gates and
            'kstrtobool(v, &a52_diag_verbose)' in gates,
            "diagnostic early_param registration")
    require('__setup("a52.mss_ssr_test=", p487_mss_ssr_test_setup);' in sysfs,
            "SSR test setup registration")
    require(b"a52.diag\x00" in native and b"a52.mss_ssr_test=\x00" in native,
            "exact linked diagnostic parameter names")
    for marker in (b"P487 TAS STREAM ch=", b"P487 TAS CALIB_UNWIND"):
        require(marker not in native, "unexpected runtime TAS marker")
    config = (pkg / ".config").read_text().splitlines()
    require("# CONFIG_COMPILE_TEST is not set" in config and
            "CONFIG_P480_A3C=y" in config and
            "CONFIG_SND_SOC_TAS256x=y" not in config, "TAS must remain dormant")
    core = Path("drivers/remoteproc/remoteproc_core.c").read_text()
    require("static bool p487_mss_sync_ssr;" in core, "sync default off")
    require("static bool autostart = true;" in Path("drivers/a52_447/a52_autostart.c").read_text(),
            "autostart default on")
    require('return "TAS_NOT_PORTED";' in Path("drivers/soc/qcom/p472_tg_audio/a3c/kona.c").read_text(),
            "QUIN filter absent")
    for line in config:
        if line.startswith("CONFIG_CMDLINE="):
            require(not any(x in line for x in ("a52.mss_sync_ssr", "a52.mss_ssr_test",
                                                "a52.diag=", "a52.autostart=",
                                                "androidboot.selinux=permissive")), "built-in cmdline conflict")
    result, before, after, effective, removed = append_exact(source, config)
    require(not any(w.split("=")[0] in ("a52.mss_sync_ssr","a52.autostart","a52.autostart_ipa","nohlt")
                    for w in after["cmdline"].split()), "unexpected override")
    report = {
        "status": "B2DIAG_PACKAGE_PASS_NO_FLASH",
        "source_commit": args.commit,
        "staged_same_build_boot_sha256": sha(source),
        "B2DIAG_boot_sha256": sha(result),
        "kernel_sha256": sha(native),
        "dtb_sha256": sha(p["dtb"]),
        "ramdisk_sha256": sha(p["ramdisk"]),
        "staged_cmdline": before["cmdline"],
        "B2DIAG_cmdline": after["cmdline"],
        "appended_args": EXTRA.decode().split(),
        "effective_first_511_bytes": effective,
        "effective_length": len(effective.encode("ascii")),
        "removed_tokens_with_proof": removed,
        "only_cmdline_bytes_changed_from_same_build_staged": True,
        "all_payloads_and_boot_id_unchanged": True,
        "sync_bridge_enabled": False,
        "tas_runtime_enabled": False,
        "hardware_log_rate_lines_per_sec": None,
        "flash_authorized": False,
    }
    out = pkg / "P487-B2DIAG.boot.img"
    out.write_bytes(result)
    (pkg / "P487-B2DIAG-repack.json").write_text(json.dumps(report, indent=2) + "\n")
    (pkg / "P487-B2DIAG-cmdline.diff").write_text(
        "--- staged (same compiled kernel)\n+++ B2DIAG\n-" + before["cmdline"] +
        "\n+" + after["cmdline"] + "\n")
    staged.unlink()
    print(json.dumps(report, indent=2))
    print("P487 B2DIAG SHA256", sha(result), "NO FLASH")

if __name__ == "__main__":
    main()

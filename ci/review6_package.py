#!/usr/bin/env python3
"""Finalize the exact same staged boot already verified by the existing P479 audits."""
import argparse, gzip, hashlib, json, subprocess
from pathlib import Path
from p480_boot_repack import parse_boot

BASE = "b6f92e1e0ca89fa637628d16f58c89ff71fb8bfb"
p = argparse.ArgumentParser()
p.add_argument("--variant", choices=["P488-CMDLINE", "P487-PROBE", "SD1"], required=True)
p.add_argument("--source", required=True)
args = p.parse_args()
pkg = Path("package/A3C")
staged = pkg / "MANGROVE-P487-STAGED-A3C-HLT.boot.img"
native = Path("out_A3C/arch/arm64/boot/Image").read_bytes()
data = staged.read_bytes()
boot = parse_boot(data)
meta = json.loads(Path("ci/P479-boot-provenance.json").read_text())
proof = json.loads((pkg / "P480-A3C-init-order-repack.json").read_text())
strings = json.loads((pkg / "P480-A3C-strings-proof.json").read_text())
def sha(value):
    return hashlib.sha256(value).hexdigest()
def git(*argv):
    return subprocess.check_output(["git", *argv], text=True).strip()

assert git("rev-parse", "HEAD") == args.source
subprocess.run(["git", "merge-base", "--is-ancestor", BASE, args.source], check=True)
changed = git("diff", "--name-only", BASE, args.source).splitlines()
assert all(proof["invariants"].values()) and proof["flash_authorized"] is False
assert strings["A3C_image_matches_compiled_Image"] and strings["flash_authorized"] is False
assert sha(data) == proof["repacked_boot_sha256"]
assert gzip.decompress(boot["kernel"]) == native
assert boot["cmdline"] == meta["cmdline"] == proof["repacked_cmdline"]
assert sha(boot["dtb"]) == meta["original_boot_dtb_sha256"]
assert sha(boot["ramdisk"]) == meta["original_ramdisk_sha256"]
assert "nohlt" not in boot["cmdline"].split()
assert "androidboot.selinux=permissive" not in boot["cmdline"].split()
assert 'return "TAS_NOT_PORTED";' in Path("drivers/soc/qcom/p472_tg_audio/a3c/kona.c").read_text()
assert "static bool autostart = true;" in Path("drivers/a52_447/a52_autostart.c").read_text()
cfg = (pkg / ".config").read_text().splitlines()
assert "CONFIG_P480_A3C=y" in cfg
if args.variant == "P488-CMDLINE":
    runtime = [x for x in changed if not x.startswith(("ci/", ".github/"))]
    assert runtime == ["arch/arm64/include/uapi/asm/setup.h"], runtime
    assert git("diff", BASE, args.source, "--", "ci/build.config") == ""
    assert "CONFIG_SND_SOC_TAS256x=y" not in cfg
    assert "CONFIG_MMC_SDHCI_MSM=y" not in cfg
elif args.variant == "P487-PROBE":
    assert "CONFIG_SND_SOC_TAS256x=y" in cfg
    assert "CONFIG_MMC_SDHCI_MSM=y" not in cfg
    assert b"P487 TAS PROBE_POWER addr=" in native and b"shutdown=1" in native
    src = Path("drivers/soc/qcom/p472_tg_audio/a3b/asoc/codecs/tas256x/os_layer/src/tas256x-regmap.c").read_text()
    assert "static bool p487_tas_enabled = true;" in src
    probe = src.split("static int tas256x_i2c_probe(", 1)[1].split("static int tas256x_i2c_remove(", 1)[0]
    for forbidden in ("tas256x_register_codec(", "tas256x_register_misc(", "register_tas256x_reset_func(", "tas256x_enable_irq(p_tas256x, true)"):
        assert forbidden not in probe, forbidden
else:
    assert "CONFIG_MMC_SDHCI_MSM=y" in cfg
    assert "CONFIG_SND_SOC_TAS256x=y" not in cfg
    for marker in (b"A52 SDHC refuse sdhc1", b"A52 SDHC R02_NATIVE_DT", b"A52 SDHC cleanup", b"A52 SDHC block ro=1"):
        assert marker in native, marker
    assert not any(x.startswith("drivers/regulator/") for x in changed)
assert not any(x.endswith((".dts", ".dtsi", ".dtb", ".dtbo")) for x in changed)
out = pkg / (args.variant + ".boot.img")
staged.rename(out)
assert sha(out.read_bytes()) == sha(data)
(pkg / (args.variant + "-cmdline.diff")).write_text("")
(pkg / (args.variant + "-source.diffstat.txt")).write_text(git("diff", "--stat", BASE, args.source) + "\n")
(pkg / "review6-provenance.json").write_text(json.dumps({
    "variant": args.variant, "base_commit": BASE, "source_commit": args.source,
    "boot_sha256": sha(data), "kernel_sha256": sha(native),
    "dtb_sha256": sha(boot["dtb"]), "ramdisk_sha256": sha(boot["ramdisk"]),
    "staged_cmdline": meta["cmdline"], "candidate_cmdline": boot["cmdline"],
    "cmdline_diff": [], "unchanged_from_same_build_staged_boot": True,
    "changed_files": changed, "flash_authorized": False
}, indent=2) + "\n")
(pkg / "README-DO-NOT-FLASH.txt").write_text(args.variant + ": review 6 candidate only. NO FLASH UNTIL CLAUDE REVIEWS. No hardware validation claimed.\n")
print(args.variant, sha(data), "PACKAGE_PROOF_PASS_NO_FLASH")

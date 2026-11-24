#!/usr/bin/env python3
"""Fail closed if real MBHC, vote ownership or diagnostic evidence is absent."""
import re
import subprocess
import sys
from pathlib import Path

out, pkg = map(Path, sys.argv[1:])
root = Path("drivers/soc/qcom/p472_tg_audio")
def symbols(path):
    return subprocess.check_output(["llvm-nm", str(path)], text=True)

proof = []
for obj, names in {
    "a3c/kona.o": ["wcd938x_mbhc_hs_detect", "wcd938x_get_codec_variant", "wcd938x_info_create_codec_entry"],
    "a3c/msm-dai-q6-v2.o": ["p484_cdc_vote_get", "p484_cdc_vote_put", "p484_cdc_vote_held"],
    "a3c/p481_diag.o": ["p484_mbhc_dump_knob"],
}.items():
    syms = symbols(out / root / obj)
    for name in names:
        assert re.search(r"\bU\s+" + name + r"$", syms, re.M), (obj, name)
        proof.append(f"{obj}: U {name}")
        assert re.search(r"\bT\s+" + name + r"$", symbols(out / "vmlinux"), re.M), name
        proof.append(f"vmlinux: T {name}")
assert re.search(r"\b[tT]\s+msm_aux_codec_init$", symbols(out / root / "a3c/kona.o"), re.M)
img = (out / "arch/arm64/boot/Image").read_bytes()
for marker in ["P484 AUX_INIT","P484 AUX_BIND","P484 COMP_INIT","P484 AUX_STATE","P484 AUX_INIT_MISSED","P484 SWR_IRQ","P484 SWR_WAKE","P484 WCD_IRQ","P484 MBHC_REGS","P484 CDC_DMA_START","P484 CDC_DMA_VOTE","P484 CDC_DMA_ROLLBACK","P484 TX_CHMAP","P484 BOLERO_ACCESS","P481 MBHC_SKIPPED","mbhc hs detect failed"]:
    assert marker.encode() in img, marker
routing = (root / "a3c/msm-pcm-routing-v2.c").read_text()
for typ, size in [("audio_cal_type_basic",16),("audio_cal_info_adm_top",20),("audio_cal_type_adm_top",36)]:
    assert f"static_assert(sizeof(struct {typ}) == {size});" in routing
    proof.append(f"compiled routing ABI: {typ}={size}")
(pkg / "P484-symbols.txt").write_text("\n".join(proof) + "\n")
print("P484 real WCD calls, CDC vote ownership, compiled ABI and diagnostic strings PASS")

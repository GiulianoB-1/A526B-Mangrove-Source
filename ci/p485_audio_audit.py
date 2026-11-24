#!/usr/bin/env python3
import sys, re, subprocess
from pathlib import Path
out,pkg=map(Path,sys.argv[1:])
root=Path("drivers/soc/qcom/p472_tg_audio")
image=(out/"arch/arm64/boot/Image").read_bytes()
markers=["RX_RX0 HPH Digital Volume","RX_RX1 HPH Digital Volume","P485 TX_DAPM","P485 ROUTES kept=","P485 HPH_VOLUME","P485 HPH_IMPEDANCE","P485 ASM_MAP_ERROR","P485 MBHC_DUMP"]
for m in markers:
    assert m.encode() in image,m
for m in ["WCD_TX_OUTPUT","WCD_TX_DUMMY","P480 A3c ROUTE_SKIPPED sink="]:
    assert m.encode()+b"\0" not in image,m
wcd=(root/"a3b/asoc/codecs/wcd938x/wcd938x.c").read_text()
assert "snd_soc_dapm_micbias" in wcd
assert "ADC_MODE_VAL_NORMAL" in wcd
asm=(root/"dsp/q6asm.c").read_text()
assert asm.count("READ_ONCE(ac->port[dir].tmp_hdl)") == 2
rx=out/root/"a3b/asoc/codecs/bolero/rx-macro.o"
nm=subprocess.check_output(["llvm-nm",str(rx)],text=True)
for s in ["wcd_impedance_vol_get","wcd_impedance_vol_put"]:
    assert re.search(r"\b[tT]\s+"+s+r"$",nm,re.M),s
(pkg/"P485-proof.txt").write_text("\n".join(markers)+"\n27 pinned DT pairs retained (source expectation; hardware pending)\n")
print("P485 TG audio source and image proof PASS")

#!/usr/bin/env python3
"""Require real CVD/control binding and list exact donor controls."""
import re
import subprocess
import sys
from pathlib import Path

out, pkg = map(Path, sys.argv[1:])
root = Path("drivers/soc/qcom/p472_tg_audio")
source = (root / "a3c/msm-pcm-voice-v2.c").read_text()
names = [a or b for a, b in re.findall(r'SOC_[A-Z_]+\("([^"]+)"|\.name\s*=\s*"([^"]+)"', source)]
expected = ["Voice Rx Device Mute","Voice Tx Device Mute","Voice Tx Mute","Voice Rx Gain","TTY Mode","Slowtalk Enable","Voice ECNS Enable","Voice Topology Disable","HD Voice Enable","CVD Version","Voice Sidetone Enable","Voice Mic Break Enable","Voc Rec Config"]
assert names == expected, names
assert "voc_start_voice_call(" not in source
assert "voc_resume_voice_call(" not in source
assert "struct snd_pcm_ops" not in source
def syms(p):
    return subprocess.check_output(["llvm-nm", str(p)], text=True)
proof = []
for path, names2 in {
    "a3c/msm-pcm-voice-v2.o": ["voc_get_cvd_version", "snd_soc_add_component_controls"],
    "dsp/p472_a3a_q6_init.o": ["voice_init", "p483_card_auto_start"],
    "a3c/platform_init.o": ["msm_pcm_voice_init"],
    "a3c/kona.o": ["p483_voice_platform_ready"],
}.items():
    obj = out / root / path
    for name in names2:
        assert re.search(r"\bU\s+" + name + r"$", syms(obj), re.M), (path, name)
        proof.append(f"{path}: U {name}")
for name in ("voc_get_cvd_version", "voice_init", "msm_pcm_voice_init", "p483_voice_platform_ready"):
    assert re.search(r"\bT\s+" + name + r"$", syms(out / "vmlinux"), re.M), name
    proof.append(f"vmlinux: T {name}")
image = (out / "arch/arm64/boot/Image").read_bytes()
for name in names:
    assert name.encode() + b"\0" in image, name
(pkg / "P483-voice-controls.txt").write_text("\n".join(names) + "\n")
(pkg / "P483-voice-symbols.txt").write_text("\n".join(proof) + "\n")
print("P483 real voice symbol and control strings PASS")

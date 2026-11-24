#!/usr/bin/env python3
"""Fail if any compiled consumer uses a stub or the linked backend is absent."""
import re
import subprocess
import sys
from pathlib import Path

out, report = map(Path, sys.argv[1:])
root = Path("drivers/soc/qcom/p472_tg_audio")
prefix = "digital_cdc_rsc_mgr_hw_vote_"
required = {"bolero-cdc", "va-macro", "pinctrl-lpi", "swr-mstr-ctrl"}
seen = set()
evidence = []
def symbols(path):
    return subprocess.check_output(["llvm-nm", str(path)], text=True)
for source in sorted(root.rglob("*.c")):
    if source.name == "digital-cdc-rsc-mgr.c":
        continue
    calls = set(re.findall(r"\b(" + prefix + r"\w+)\s*\(", source.read_text(errors="replace")))
    if not calls:
        continue
    obj = out / source.with_suffix(".o")
    if not obj.exists():
        assert source.stem not in required, f"Missing required object: {obj}"
        continue
    seen.add(source.stem)
    syms = symbols(obj)
    for name in sorted(calls):
        assert re.search(r"\bU\s+" + name + r"$", syms, re.M), (str(obj), name)
        evidence.append(f"{obj}: U {name}")
assert required <= seen, required - seen
backend = out / root / "dsp/digital-cdc-rsc-mgr.o"
for obj in (backend, out / "vmlinux"):
    syms = symbols(obj)
    for suffix in ("enable", "disable", "reset", "held"):
        name = prefix + suffix
        assert re.search(r"\bT\s+" + name + r"$", syms, re.M), (str(obj), name)
        evidence.append(f"{obj}: T {name}")
report.write_text("\n".join(evidence) + "\nP482 SYMBOL_PROOF PASS\n")
print(report.read_text())

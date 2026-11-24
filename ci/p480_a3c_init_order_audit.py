#!/usr/bin/env python3
"""A3c built-in initcall and dependency-order audit, without modifying source.

Reports every explicit init registration under a3c; refuses regressions that
could run calibration/Q6/platform/machine binding at device_initcall.
"""
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
a3c = root / "drivers/soc/qcom/p472_tg_audio/a3c"
a3a = (root / "drivers/soc/qcom/p472_tg_audio/dsp/p472_a3a_q6_init.c").read_text()
a2a = (root / "drivers/soc/qcom/p472_tg_audio/dsp/p472_a2a_core_init.c").read_text()
cal = (root / "drivers/soc/qcom/p472_tg_audio/dsp/audio_calibration.c").read_text()
pattern = re.compile(r"\b(module_init|module_platform_driver|module_i2c_driver|module_spi_driver|"
                     r"builtin_platform_driver|(?:early|pure|core|postcore|arch|subsys|fs|rootfs|"
                     r"device|late)_initcall(?:_sync)?)\s*\(\s*([^)]+)\)", re.M)
entries = []
for path in sorted(a3c.rglob("*.c")):
    source = path.read_text(encoding="utf-8")
    for match in pattern.finditer(source):
        entries.append({
            "file": str(path.relative_to(root)),
            "line": source.count("\n", 0, match.start()) + 1,
            "macro": match.group(1),
            "entry": match.group(2).strip(),
            "level": "device_initcall (6) for built-in module registration"
                     if match.group(1).startswith("module_") else match.group(1),
        })
expected = [{
    "file": "drivers/soc/qcom/p472_tg_audio/a3c/codecs/msm_stub.c",
    "macro": "module_init", "entry": "msm_stub_init",
}]
actual = [{k:v for k,v in entry.items() if k in ("file", "macro", "entry")}
          for entry in entries]
assert actual == expected, ("Unexpected early A3c initcall(s)", actual)
platform = (a3c / "platform_init.c").read_text()
machine = (a3c / "kona.c").read_text()
stub = (a3c / "codecs/msm_stub.c").read_text()
assert "int __init p480_audio_platform_init(void)" in platform
diag = (a3c / "p481_diag.c").read_text()
assert "int p480_kona_machine_init(void)" in machine
assert "int __init p480_kona_machine_init(void)" not in machine
assert "p480_kona_machine_init(" not in a3a
assert "rc = p480_kona_machine_init();" in diag
assert "__ATTR(card_enable, 0200, NULL, card_enable_store)" in diag
assert "return platform_driver_register(&msm_stub_driver)" in stub
assert "audio_cal_register(" not in stub and "q6asm_" not in stub
assert "late_initcall(p472_a2a_init)" in a2a
assert "late_initcall_sync(p472_a3a_start)" in a3a
assert "p472_a2a_ready()" in a3a and "audio_cal_is_initialized()" in a3a
assert "CAL_REGISTER_SKIPPED reason=not_initialized" in cal
assert "return -ENODEV;" in cal
assert all(word in a3a for word in (
    "P480 A3c PLATFORM_SKIPPED reason=a2a_not_ready",
    "P480 A3c PLATFORM_SKIPPED reason=a3a_services_failed",
    "rc = p480_audio_platform_init();",
    "rc = p481_diag_register();",
))
assert (a3a.index("rc = q6lsm_init();")
        < a3a.index("rc = p480_audio_platform_init();")
        < a3a.index("rc = p481_diag_register();"))
report = {
    "result": "PASS",
    "baseline_a2a": {"file":"dsp/p472_a2a_core_init.c", "level":"late_initcall (7)"},
    "baseline_a3a": {"file":"dsp/p472_a3a_q6_init.c", "level":"late_initcall_sync (7s)"},
    "a3c_registration_macros": entries,
    "a3c_platform": "explicit in A3a after q6lsm; no standalone initcall",
    "a3c_machine": "one-shot root card_enable after providers; no boot machine registration",
    "msm_stub": "device_initcall (6); registration-only stub codec, no cal/Q6 use",
    "total_a3c_c_files_scanned": len(list(a3c.rglob("*.c"))),
    "no_a3c_config": "All cal/A2a/A3a changes guarded by CONFIG_P480_A3C",
    "flash_authorized": False,
}
out = root / "p480-init-order-audit.json"
out.write_text(json.dumps(report,indent=2)+"\n")
print(json.dumps(report,indent=2),flush=True)

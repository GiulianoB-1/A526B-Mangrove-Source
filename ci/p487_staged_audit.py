#!/usr/bin/env python3
"""Source and linked-image checks for partial P487, never hardware proof."""
import sys
from pathlib import Path
out,pkg=map(Path,sys.argv[1:])
w=Path("drivers/a52_447/a52_autostart.c").read_text()
s=Path("drivers/a52_447/a52_dsp_scm_runtime.c").read_text()
i=Path("drivers/platform/msm/ipa/ipa_v3/ipa.c").read_text()
r=Path("drivers/remoteproc/remoteproc_core.c").read_text()
y=Path("drivers/remoteproc/remoteproc_sysfs.c").read_text()
d=Path("drivers/soc/qcom/p472_tg_audio/a3c/p481_diag.c").read_text()
img=(out/"arch/arm64/boot/Image").read_bytes()
for m in ("P487 AUTOSTART step=", "P487 AUTOSTART done", "a52.autostart=", "a52.autostart_ipa=", "a52_audio.debug=", "P486 CARD_AUTO source=manual_knob", "P487 CARD_RETRY"):
    assert m.encode() in img, m
for m in ("P487_MIN_UPTIME_MS 12000", "P487_DEADLINE_MS 120000", "P487_POLL_MS 200", 'filp_open("/vendor/firmware_mnt/image/adsp.mdt", O_RDONLY, 0)', "kernel_read(file", "__pm_stay_awake(autostart_ws)", "__pm_relax(autostart_ws)"):
    assert m in w, m
assert w.index("rc = a52_p487_scm_enable_once();") < w.index("rc = a52_p487_ipa_arm_once(true);") < w.index("rc = a52_p487_adsp_start_once(rproc);")
assert "qcom_scm_is_available()" in w
assert "ret = a52_p487_scm_enable_once();" in s and "ret = a52_p458_create_scm_once();" in s
assert "ret = a52_p487_ipa_arm_once(false);" in i and "atomic_cmpxchg(&p470_a4_armed, 0, 1)" in i
assert "ret = a52_p487_adsp_start_once(rproc);" in y and "ret = rproc_boot(rproc);" in r
assert "p487_adsp_attempted = true;" in r and "qcom,sm6350-adsp-pas" in r
assert "static bool p487_debug;" in d and "__ATTR_RW(debug)" in d
(pkg/"P487-staged-proof.txt").write_text("P487 partial source + compiled marker proof; no TAS, no hardware pass, NO FLASH\n")

#!/usr/bin/env python3
"""P486 source wiring and compiled diagnostic proof; not hardware evidence."""
import sys, re
from pathlib import Path
out,pkg=map(Path,sys.argv[1:])
root=Path("drivers/soc/qcom/p472_tg_audio")
w=(root/"a3b/asoc/codecs/wcd938x/wcd938x.c").read_text()
d=(root/"a3c/p481_diag.c").read_text()
a=(root/"ipc/apr.c").read_text()
image=(out/"arch/arm64/boot/Image").read_bytes()
for marker in ("P486 TX_SEQ step=%d op=reg_write","P486 TX_SEQ step=%d op=connect_port",
               "P486 TX_SEQ step=%d op=datapath","P486 TX_SEQ step=%d op=clk_rate",
               "P486 CARD_AUTO source=apr_loaded"):
    assert marker.encode() in image, marker
for n in range(1,5):
    assert re.search(r'SND_SOC_DAPM_ADC_E\("ADC%d"[^\n]*\n\s*p486_amic_adc_event' % n,w)
    assert re.search(r'P486_TX_EARLY_MIXER\("ADC%d REQ"[^\n]*\n\s*NULL, 0, p486_amic_req_event' % n,w)
    assert re.search(r'SND_SOC_DAPM_MIXER_E\("ADC%d_MIXER"[^;]*?wcd938x_enable_req' % n,w)
for n in range(1,9):
    assert re.search(r'SND_SOC_DAPM_ADC_E\("DMIC%d"[^\n]*\n\s*p486_dmic_adc_event' % n,w)
    assert re.search(r'P486_TX_EARLY_MIXER\("DMIC%d_MIXER", SND_SOC_NOPM, %d,[^;]*?p486_dmic_mixer_event' % (n,n-1),w)
assert ".subseq = -1" in w
for name,up,down in (
    ("p486_amic_adc_event","wcd938x_tx_swr_ctrl","wcd938x_codec_enable_adc"),
    ("p486_amic_req_event","wcd938x_codec_enable_adc","wcd938x_tx_swr_ctrl"),
    ("p486_dmic_adc_event","wcd938x_tx_swr_ctrl","wcd938x_codec_enable_dmic"),
    ("p486_dmic_mixer_event","wcd938x_codec_enable_dmic","wcd938x_tx_swr_ctrl")):
    body=w[w.index("static int "+name):].split("\n}",1)[0]
    assert re.search(r'if \(event == SND_SOC_DAPM_PRE_PMU\)\s*return '+up+r'\(w, kcontrol, event\);\s*return '+down,body)
assert "auto_checks" not in d and "DECLARE_WORK(p483_card_job" in d
assert "apr_get_q6_state() != APR_SUBSYS_LOADED" in d
assert "p486_card_adsp_up();" in a and "p486_card_adsp_up();" in d
assert a.index("snd_event_notify(apr_priv->dev, SND_EVENT_UP);") < a.index("p486_card_adsp_up();")
(pkg/"P486-proof.txt").write_text("P486 static callback wiring and image markers PASS; TG stop tie and actual trace pending; NO FLASH\n")
print("P486 static source and image proof PASS")

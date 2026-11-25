#!/usr/bin/env python3
"""P487 compile-only controls: prove actual TG object strings, not runtime card."""
import sys
from pathlib import Path
root=Path(sys.argv[1])
codec=(root/"os_layer/src/tas256x-codec.o").read_bytes()
reg=(root/"os_layer/src/tas256x-regmap.o").read_bytes()
physical=(root/"physical_layer/src/tas256x.o").read_bytes()
for name in ("TAS256X DAC Mute", "TAS256X ASI1 SEL LEFT", "TAS256X ASI1 SEL RIGHT",
             "TAS256X ASI Left", "TAS256X ASI Right", "Switch",
             "I2C offset", "Left", "Right", "LeftRightDiv2"):
    assert name.encode()+b"\0" in codec, name
# ASoC generates the full names of DAPM switches from widget + control.
for name in ("TAS256X RECEIVER ENABLE LEFT", "TAS256X RECEIVER ENABLE RIGHT",
             "TAS256X AMP OUTPUT LVL LEFT", "TAS256X AMP OUTPUT LVL RIGHT",
             "TAS256X PLAYBACK VOLUME LEFT", "TAS256X PLAYBACK VOLUME RIGHT",
             "TAS256X BOOST CURRENT LEFT", "TAS256X BOOST CURRENT RIGHT",
             "TAS256X BOP ENABLE LEFT", "TAS256X BOP ENABLE RIGHT",
             "ENABLE", "DISABLE"):
    assert name.encode()+b"\0" in reg, name
assert b"P487 TAS RECEIVER ENABLE" in physical
print("P487 TG TAS object strings PASS; 14 requested names, donor also has right receiver; no hardware proof")

assert b'P487 TAS STREAM ch=' in codec
assert b'P487 TAS CALIB ch=' in (root/'algo/src/tas25xx-calib-validation.o').read_bytes()

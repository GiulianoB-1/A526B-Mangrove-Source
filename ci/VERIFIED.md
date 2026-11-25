# Hardware-verified source milestones

## Standing promotion policy

- Promote only phone-tested functionality reported by Giordano and reviewed by Claude. A successful build alone is not hardware confirmation.
- Fast-forward main to the verified source, or use a normal merge commit if history diverged; never rewrite or force-push main.
- Tag the exact hardware-tested source commit `verified/<phase>-<feature>`, record the boot SHA256 and limitations here, and require green CI on main.
- Start subsequent work branches from updated main; keep unconfirmed experiments on feature branches.
- Do not change kernel code, remove diagnostics, or reformat during promotion. Diagnostic cleanup is separately reviewed. NO FLASH remains in force until review.

## P485: headphone jack and IPA

- Recorded: 2026-11-25 (hardware confirmation reported by Giordano and Claude in the promotion instruction).
- Tested source: `8f2008903bec32cfd5b0ce0cb42cf2415f51c9bf`
- Verification tag: `verified/p485-headphone-jack-and-ipa` (points to tested source, not this documentation commit).
- Original successful CI: https://github.com/GiulianoB-1/A526B-Mangrove-Source/actions/runs/45414153140
- Boot image: `MANGROVE-P485-A3C-HLT.boot.img`
- Boot image SHA256: `6b1ea921d07c440e74277f07a6d243e5bde5b2bb3fc6dff4385b3a5d160a82e0` (CI log and SHA256SUMS check).
- Artifact: https://github.com/GiulianoB-1/A526B-Mangrove-Source/actions/runs/45414153140/artifacts/14931813996

### Confirmed on hardware

- Sound card registers.
- Headphone jack detection: MBHC to Android h2w.
- HAL music playback on the 3.5 mm jack, normal volume and volume keys (IMPED_SENSING HPH Digital Volume).
- ADSP+IPA mode alongside audio: modem running, Cellcom LTE, rmnet_ipa0 packets.

### Not working or not confirmed

- Microphone silent; P486 candidate fix is awaiting hardware validation.
- Speaker/TAS not ported.
- Card auto-start can miss a slow ADSP; P486 candidate fix is awaiting hardware validation.
- Voice Recorder storage EACCES.

Main rebuild is required after promotion; it is a separate build, not the hardware-tested boot image. The P485 limitations above are historical; see P486 below.

## P486: main microphone and APR card auto-start

- Recorded: 2026-11-25; phone results reported by Giordano and reviewed by Claude.
- Tested source: `585e7d062db0715e42a1b5bd366aed43031d29f2`
- Verification tag: `verified/p486-mic` (exact tested commit).
- Successful original CI: https://github.com/GiulianoB-1/A526B-Mangrove-Source/actions/runs/45625443705
- Boot image: `MANGROVE-P486-A3C-HLT.boot.img`
- SHA256: `91dfc326c52bb71c3fece2a07bb3901bf2b980c1dbfb82b276ad265706526087` (original CI log, SHA256SUMS passed).
- Artifact: https://github.com/GiulianoB-1/A526B-Mangrove-Source/actions/runs/45625443705/artifacts/15029670811

### Confirmed on hardware

- Main microphone records real audio through Samsung Voice Recorder/HAL and playback is audible.
- WCD938x TX path: MIC BIAS → ADC1 → SoundWire TX → TX_CDC_DMA_TX_3 → ADSP. Reported TX trace start: micbias, enable_adc, connect_port (step 11), enable_req, clk_rate, datapath enable (step 19); clean shutdown steps 21–29.
- Sound card auto-starts at boot via the APR LOADED event (approximately 15 s), without any app or manual knob: CARD_AUTO at 13.831206 s, source=apr_loaded at 13.831222 s, CARD_ENABLE rc=0 at 13.833282 s, SND_CARD_REGISTER rc=0 at 15.116817 s; /proc/asound/cards reports litolagoonmtpsn.
- P485 headphone jack detection, headphone playback and HPH volume, ADSP+IPA modem/LTE coexistence retained, per hardware review.

### Not working or not confirmed

- Loudspeaker (TAS256x on QUIN MI2S) and earpiece not ported.
- Voice-call audio not confirmed.
- Headset-mic (ear-mic) not tested.
- Voice Recorder file-save EACCES (storage/MediaProvider suspected, separate from audio).

Main CI must pass; its rebuilt image is not the original tested artifact. P487 remains experimental until hardware-confirmed and reviewed. NO FLASH.

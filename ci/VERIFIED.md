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
- Verification tag requested: `verified/p485-headphone-jack-and-ipa` (points to tested source, not this documentation commit).
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

Main rebuild is required after promotion; it is a separate build, not the hardware-tested boot image. P486 must stay unmerged until hardware confirmation and Claude review.

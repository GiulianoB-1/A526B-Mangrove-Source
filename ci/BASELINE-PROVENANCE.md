# P479 verified Mangrove baseline

Complete source: Phase446WX artifact 12271681215 and the subsequent proven P447
through P479 source changes. Original run: 43638159300 at
e2e9f3fad600489deb8fa0816245a87cef6b9b95.
Evidence: ci/P479-equivalence-proof.json, with exact .config, System.map
symbols/addresses and allocated ELF section sizes plus compiled feature markers.
Source archive: legacy release phase446wx-source-backup.
Reference binaries: legacy release p479-equivalence-reference.

Permanent comparison proof: release p479-equivalence-evidence-44026270014.
Original P479 proof run: 44026270014 (source producer commit
5e5db938a24068ae5a804b003e5cbe16d7f31438).

IMPORTANT: strict equivalence proves exact .config, System.map symbol/addresses,
allocated ELF section sizes and required kernel Image markers, but the original
and rebuilt raw Image SHA256 differ. Original c1e29a64f611f402cdc8e5db1f42fea5b6ec9f48b19df60d5b9aa6da2d8585ae;
rebuilt 04bec126c1ae6d06e4ac43f417d1a7dfd22fa82638f995d41721f27a056a1bc7
from the authenticated full-source producer run 44038773282. The previous
strict-PASS build 44026270014 had a different rebuilt Image SHA256
2b094dc42cd6079ab5dd2092a69a68b2ae6a3676ffe8da7215db84f78a8c1070.
Do NOT report byte-for-byte Image identity, or infer flash authorization.
The verified source release tag points at main HEAD solely for GitHub workflow
file permissions; the actual source archive and proof identify its producer.
Verified source release producer: 44038773282, commit
5e6fb4713022f22b417a9e6581cf3c5d9c2b16f5.
Full source archive SHA256:
43ecd5698f319328290721c4f79d1dddfe7e8acb30f4ba1c3a47629f2115d9e3.


The archived ReSukiSU/KernelSU Kbuild originally required a separate nested
.git submodule and computed version 35171 from 4471 commits. Its exact original
Kbuild is preserved in ci/P479-original-KernelSU.Kbuild (SHA256 pinned).
The direct-source adaptation removes that prerequisite and retains the exact
compiled historical full version v4.2.0-rc3-239e1e88-dirty@ReSukiSU.
The adaptation report is ci/P479-KernelSU-direct-source.json. Future KernelSU
version changes require ordinary direct Git source commits, not .git submodules.

P479 generated build provenance is deliberately fixed using ci/P479-compile.h,
ci/P479-historical-manifest.h, and ci/P479-kheaders.tar.xz. The distinct real
Phase446WX fingerprint is separately preserved in ci/P479-native-WX-manifest.h.
The two fingerprint values are not interchangeable and MUST remain documented.

The entire kernel source, including KernelSU, is stored as one ordinary Git
commit and tagged p479-baseline. Future changes such as P480 A3c must be direct
source commits. No generated patch chains are used at build time.
The preserved ci/P479.config + ci/P479-final-tree-manifest.json record the
baseline. The active ci/build.config + ci/build-manifest.json may change in
future direct source commits, starting with P480 A3c.
NO FLASHING without separate authorization.

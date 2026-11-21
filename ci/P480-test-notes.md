# P480 reviewer build and P479 boot-template controls

The **only** authorized template for a review-only P480 repack is the exact
P479 H4-only HLT boot image from run 43638159300, artifact 14131467025,
permanent release `p479-equivalence-reference`. The exact hashes are pinned
in `ci/P479-boot-provenance.json` and `ci/P479-boot-dtb.sha256`.
The older Phase447C/WX `boot-f0bisect-gki-p6-v1.img` must never be reused
for P480. P479 changed its embedded boot DTB after 447C via the ten stages
listed in the provenance file. The archived P479 original Image hash is
`c1e29a64f611f402cdc8e5db1f42fea5b6ec9f48b19df60d5b9aa6da2d8585ae`.
The P479 equivalent source proof covers kernel Image structural equivalence,
not boot.img. Consequently, repacking must compare the original P479 boot.img,
not an earlier base. No changes to DTB or ramdisk are permitted.

P479 baseline is `p479-baseline` at commit
`a040e6cf1e7232fc3e91516416fb7fffa3fc9025`.
The common P480 branch also includes **P479 direct-source build repair commit
`55babd9c651375580b3c5c10d7f37ac6213e005c`**. In
`drivers/a52_pil/subsys-pil-tz.c`, this changes
`#include <soc/qcom/subsystem_restart.h>` to
`#include "../../a52-compat/include/soc/qcom/subsystem_restart.h"`.
The purpose is matching the legacy PIL/SSR descriptor and shutdown-ack ABI
used by `subsystem_restart.c`. This is a real source change vs the original
P479 tag, **not** a change enabled by CONFIG_P480_A3C.
Both P480 A3C and NO-A3C build variants contain it. Review its behavior
separately; do not label either as identical to the untouched P479 source.

The control build currently compiles **the later repaired source at commit
`55babd9c651375580b3c5c10d7f37ac6213e005c`**, not the pristine
immutable `p479-baseline` source. That distinction matters: the control
repack preserves the exact P479 final HLT boot DTB, ramdisk and cmdline,
but the control is NOT a second P479 kernel equivalence proof. The
separate accepted P479 equivalence proof remains in
`ci/P479-equivalence-proof.json` and is tied to `p479-baseline`.

The corrected P480 twin CI run 44809264636 reported:
- P479 repaired-control `.config` SHA256
  `3d2b45be575df782a1f28d100c8f10705e5e14d9c50273cf51fa2757568e4746`
  (not identical to P479 original).
- P479 repaired-control `System.map` SHA256
  `b535694bad570d653bd684b1c22eff6186c19bec81cac06c63ad0dfda5eb763a`
  (not identical to P479 original).
- Rebuilt repaired-control native Image SHA256
  `fea488cd5e1382f13e283bf3008dd8ac0e9c22125d1027040d69898d6030838d`.
  Compared with original P479, 23,948,157 of 66,687,488 Image bytes differ;
  first difference is at offset 66008. These differences are **not proven
  to be build stamps only**. Review the PIL/SSR path separately.

The kernel-only repacker can update the kernel size and boot ID in the header,
but no other boot payload or command-line changes are allowed. No control
repack result is equivalent to hardware validation or flash authorization.

A3C is opt-in, with default-off H4 and no voice/TAS/WSA/DisplayPort codecs.
P480 runtime behavior is not tested. Both output images and the control are
**review-only, NO FLASH pending Claude approval**.

## Claude revision: original P479 provenance and duplicate-header risk

The later Claude review ACCEPTS P480 A3C symbol comparison D, including code changes in q6asm, copy helpers, static aggregates, and the AArch64 erratum veneers. This does not authorize a flash.

**P479 kernel safety correction:** the original P479 `vmlinux` recovered from permanent artifact 14131467025 has the compatibility `struct subsys_desc` with IRQ handlers and size 464 bytes. It generates an ARM64 Image and System.map matching the original P479 SHA256 pins exactly, despite the misleading `P471-D1-DATARMNET-CANDIDATE` filename. Thus the original hardware P479 ABI was the correct compat layout; the `55babd9` explicit include is a **direct-source clean-build workaround**, not a proven original P479 runtime bug. Keep the workaround while diagnosing the header search paths.

The source contains exactly 24 differing header duplicates between `include/` and `a52-compat/include/`. The original reference ZIP has no `.o.cmd` records, so its historical compiler header selection cannot be established from those absent files. A clean `.o.cmd` audit and actual original P479 ELF versus control `bloat-o-meter` audit are tracked under run 44907289642. Do not claim full equivalence without analyzing those results.

Exact original-P479-template A3C and NO-A3C boot images were ALREADY produced and independently cross-checked against the original P479 boot SHA/DTB, ramdisk and cmdline in run 44809264636. Only kernel size and boot-ID header fields change. They remain review-only, NO FLASH.

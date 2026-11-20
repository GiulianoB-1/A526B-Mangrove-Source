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

The control build compiles the immutable `p479-baseline` source separately
and repacks its rebuilt Image with this original P479 HLT boot template.
It must report Image differences honestly. Unless the raw Image diff has been
classified, do not claim that any kernel byte differences are *only* build
stamps. Boot header bookkeeping (kernel_size and boot ID) may change with the
kernel; all non-kernel payloads and command-line bytes remain identical.

A3C is opt-in, with default-off H4 and no voice/TAS/WSA/DisplayPort codecs.
P480 runtime behavior is not tested. Both output images and the control are
**review-only, NO FLASH pending Claude approval**.

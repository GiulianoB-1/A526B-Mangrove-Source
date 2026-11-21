# P480 four-source kernel comparison: reviewer handoff

> **Reviewer update, November 21, 2026:** Claude ACCEPTED comparison D, including the copy helpers, session state, q6asm conditional deltas, aggregated static tables, and linker-generated erratum veneers. The interpretation below represents the earlier review stage. Crucially, the original P479 hardware kernel had the COMPATIBLE 464-byte `subsys_desc` layout, verified directly in original P479 vmlinux DWARF, so `55babd9` is a clean direct-source include-selection workaround, NOT proof of a hardware ABI defect. The original P479 ELF was RECOVERED from reference artifact 14131467025 as `phase447b-variants/P471-D1-DATARMNET-CANDIDATE.vmlinux`. Despite its P471 name, its ELF-generated raw Image SHA256 `c1e29a64…` and System.map SHA256 `26ae9bda…` exactly equal original P479. Therefore B IS MEASURABLE using that actual ELF, and the earlier 'B impossible' statement is superseded. Follow-up source-duplication and historical B audit: [run 44907289642](https://github.com/GiulianoB-1/A526B-Mangrove-Source/actions/runs/44907289642). NO FLASH. 


**Status:** CI comparison completed, but **NOT APPROVED FOR FLASH**. This is a source and linked-symbol-size review, not a runtime or ABI proof.

**Reproducible evidence:** [GitHub Actions run 44880139058](https://github.com/GiulianoB-1/A526B-Mangrove-Source/actions/runs/44880139058), source commit `4652ba4d7d08956e4af9d2911e44e1a4811e3180`. Artifact `P479-P480-FOUR-SOURCE-SYMBOL-EVIDENCE-4652ba4d7d08956e4af9d2911e44e1a4811e3180`, ID **14699652122**, contains every generated `.config.diff`, the actual C/D `scripts/bloat-o-meter -c` output, compiler diagnostic, JSON and SHA256SUMS. All artifact SHA256SUMS were verified.

## A. Untouched P479 at a040e6cf1e7232fc3e91516416fb7fffa3fc9025

- The worktree was checked out at the immutable tag without applying source changes.
- `olddefconfig` passed, but the compile of `drivers/a52_pil/subsys-pil-tz.o` **FAILED** before linking. The compiler reports nine errors, including absent `struct subsys_desc` IRQ-handler fields and a mismatched `complete_shutdown_ack()` argument type.
- The pristine P479 `vmlinux` and new `System.map` **do not exist** in this environment. No equality conclusion can be drawn against original P479 `System.map` SHA256 `26ae9bda166d764bf695d6c51b8799632bc6f1ea276747a9f8fd1d0caf6d3a3e`.
- Its `olddefconfig` emits only these three lines relative to P479's committed `ci/build.config`:
  - `CONFIG_CC_CAN_LINK=y`
  - `CONFIG_CC_CAN_LINK_STATIC=y`
  - `# CONFIG_UAPI_HEADER_TEST is not set`
- The compiler failure is direct evidence that the header mismatch is a build defect, consistent with Claude's validation of the `55babd9` PIL/SSR fix.

## B. Untouched P479 versus 55babd9

- The rebuilt `.config` files are **byte-identical** after `olddefconfig` (both SHA256 `3d2b45be575df782a1f28d100c8f10705e5e14d9c50273cf51fa2757568e4746`).
- **No real bloat-o-meter result exists**: the immutable P479 source cannot link, so replacing this missing result with a comparison from a patched source would be misleading.
- The repaired control `55babd9` compiled and linked. Its `System.map` SHA256 in this run was `ecc901979b436118859cd7263f277b38932965060093fa2f8e73ba2b76cb4cb6`. It differs from the original P479 pin.
- Preserve the genuine `drivers/a52_pil/subsys-pil-tz.c` compatibility-header fix. **Do not silently modify or retag the P479 baseline.**

## C. 55babd9 versus P480 NO-A3C

- **PASS for symbol-size comparison:** `bloat-o-meter -c` reports **0 added, 0 removed, 0 grown, 0 shrunk** symbols across text, data and RO data. Symbol-size totals are equal.
- `out_CONTROL/.config` and `out_NO-A3C/.config` differ by **one line only**: the latter explicitly adds `# CONFIG_P480_A3C is not set`. There is no newly enabled feature.
- This is a strong build regression check, **not** bit-for-bit binary equivalence or behavioral proof. `System.map` hashes may differ from build metadata/layout; the symbol sizes do not.

## D. P480 NO-A3C versus P480 A3C

`bloat-o-meter -c` reports **1,997 changed-size symbol entries**:

| Section | Newly added | Existing size changes | Removed | Net symbol-size change |
|---|---:|---:|---:|---:|
| Functions | 726 | 8 | 2 | +199,744 bytes |
| Data | 1,258 | 3 | 0 | +4,022,436 bytes |
| RO data | 0 | 0 | 0 | 0 |
| **Total** | **1,984** | **11** | **2** | **+4,222,180 bytes** |

Only `CONFIG_P480_A3C` changes between these two compiled `.config` files. The 1,984 newly added symbol entries overwhelmingly resemble native A3C machine, DAI, PCM, routing, effects and codec/provider functionality.

**Eleven existing symbol entries change size, so it is inaccurate to claim that every change is simply a newly added A3C symbol:**

| Existing symbol | Size difference | Reviewer context |
|---|---:|---|
| `_copy_to_user` | +1,344 B | Generic/common name, needs object-level attribution |
| `_copy_from_user` | +1,188 B | Generic/common name, needs object-level attribution |
| `q6asm_session_free` | +8 B | Shared native DSP `q6asm.c`, A3C conditional changes are present |
| `q6asm_srvc_callback` | +4 B | Shared native DSP `q6asm.c` |
| `q6asm_set_pp_params` | +4 B | Shared native DSP `q6asm.c` |
| `q6asm_send_stream_cmd` | +4 B | Shared native DSP `q6asm.c` |
| `q6asm_callback` | +4 B | Shared native DSP `q6asm.c` |
| `__q6asm_add_hdr` | +4 B | Shared native DSP `q6asm.c` |
| `.compoundliteral` | +639,108 B | Aggregated compiler-generated data name, not a single attributable object |
| `session` | +128 B | `q6asm.c` changes `struct audio_session` under `#ifdef CONFIG_P480_A3C`, adding `bool ignore` |
| `cal_data` | +16 B | Shared name; requires object-level attribution |

Two additional address-containing `__CortexA53843419_...` function-name entries disappear (8 B each). Their names suggest address-dependent generated entries, but the linker's ownership and impact **have not been verified**.

The [committed shared DSP source](https://github.com/GiulianoB-1/A526B-Mangrove-Source/blob/4652ba4d7d08956e4af9d2911e44e1a4811e3180/drivers/soc/qcom/p472_tg_audio/dsp/q6asm.c) explicitly contains `#ifdef CONFIG_P480_A3C` at several locations. In particular, `struct audio_session` adds an `ignore` field and A3C enables session-ID retry and APR re-registration support. Therefore some existing DSP function and data changes are expected, but this **does not independently classify every changed symbol**.

The [native A3C Makefile](https://github.com/GiulianoB-1/A526B-Mangrove-Source/blob/4652ba4d7d08956e4af9d2911e44e1a4811e3180/drivers/soc/qcom/p472_tg_audio/a3c/Makefile) links the additional audio providers when enabled.

## Required next reviewer decision

1. **Accept or reject the A/B limitation:** pristine P479 is unbuildable because of the now-confirmed PIL/SSR header defect. Treat B as unmeasurable, not passed.
2. **Confirm whether C's all-zero symbol-size diff** and the sole disabled-config comment satisfy the compatibility gate.
3. **Classify D's 11 existing size changes and two removed generated-name entries**, especially `_copy_to_user`, `_copy_from_user` and the shared `q6asm.c` changes. If needed, request an object-level `nm` / linker-map attribution before approving.
4. Independently review the previously built final-P479-template review images and their DTB/ramdisk/cmdline preservation checks from [run 44809264636](https://github.com/GiulianoB-1/A526B-Mangrove-Source/actions/runs/44809264636).

**NO FLASH. Hardware audio not tested. PR #2 not merged.** This report contains no authorization to flash or arm charging/shadow features.

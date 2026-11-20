# GitHub Actions: one build workflow and one source audit

**direct-source-build.yml** is the only kernel compile and boot packaging workflow.
Normal source commits compile one direct-source kernel. Pull requests are
audited, not compiled a second time. To build paired P480 reviewer images,
select **Run workflow** on `feature/p480-a3c-native` and set the boolean
`p480_twins` to true. This runs the separate, mutually exclusive job inside
the same workflow to build A3C, NO-A3C and the P479 control.
Both outputs use the original final-P479 HLT boot template; kernel-only
repack, cmdline, ramdisk and DTB controls must pass. **NO FLASH**.

**source-safety-audit.yml** contains the legacy P479 source gates, P480
A3c native source audit, TG r02 overlay pin, and 27-pair live source check.

The removed phase-specific YAMLs were one-time provenance, outdated WX
archive handling or duplicate build/audit entry points. Historical runs
and artifacts remain available. Successful P480 P479-template review run:
https://github.com/GiulianoB-1/A526B-Mangrove-Source/actions/runs/44809264636

Original P479 release (permanent):
https://github.com/GiulianoB-1/A526B-Mangrove-Kernel/releases/tag/p479-equivalence-reference

Immutable source tag `p479-baseline` and all existing kernel C source files
are unchanged. The PIL/SSR header repair in 55babd9 is documented in
`ci/P480-test-notes.md`. Hardware testing and flash approval remain pending.

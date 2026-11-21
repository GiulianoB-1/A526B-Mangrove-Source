# P479 original-ELF versus direct-source: completed review evidence

**Status: review required, NO FLASH.** Data from [successful CI run 44907289642](https://github.com/GiulianoB-1/A526B-Mangrove-Source/actions/runs/44907289642), evidence artifact `14709608584`. The generated SHA256SUMS was verified. No new kernel source modification is proposed by this report.

## B: authoritative original P479 vs 55babd9 control
The original P479 reference ZIP `14131467025` contains a misnamed `P471-D1-DATARMNET-CANDIDATE.vmlinux`. Its extracted Image SHA256 `c1e29a64f611f402cdc8e5db1f42fea5b6ec9f48b19df60d5b9aa6da2d8585ae` and associated System.map SHA256 `26ae9bda166d764bf695d6c51b8799632bc6f1ea276747a9f8fd1d0caf6d3a3e` exactly match the original P479 pins. This **makes B directly measurable**.

The original vs repaired control `scripts/bloat-o-meter -c` output has five entries:

| Type | Symbol | Original | Control | Delta |
|---|---|---:|---:|---:|
| Function | `ddebug_exec_queries` | 2996 | 2820 | -176 |
| Function | `ddebug_proc_show` | 340 | 300 | -40 |
| Function | `__CortexA53843419_FFFFFFC008F18004` | 8 | absent | -8 |
| Function | `__CortexA53843419_FFFFFFC0091C4004` | absent | 8 | +8 |
| Data | `vermagic` | 61 | 62 | +1 |

The erratum-843419 symbol is address-dependent. `vermagic` has a 1-byte length change, but its actual bytes should be inspected before attributing this entirely to metadata. **The two dynamic-debug function-size changes remain unexplained**, and cannot be dismissed as build stamps.

`System.map` has 164,730 entries in each linked output. Original vs 55babd9 contains 105,356 aligned line differences: 104,845 address-only, 511 symbol name/order. The control SHA256 is `e370020b718cdc4019422e383daf05f55651e3c08c0354a3ff57296f72c69c21`. The full name/order diff and all 105,356 differences are in artifact 14709608584. The name/order drift is mostly generated suffixes and KVM local entries. **Exact System.map equality has not been reached**.

`55babd9` versus P480 NO-A3C has **zero `bloat-o-meter` size deltas** and identical symbol name/order, despite 60,623 address-only differences (control map `e370020b...`, NO-A3C map `9a43ddcb...`).

The original, repaired, and NO-A3C `pahole` layouts all show `subsys_desc` size **464**, and matching IRQ-handler offsets. Original P479 uses the compatibility layout. The `55babd9` header include is a clean direct-source build workaround, not an original hardware ABI bug.

## 24 conflicting duplicate headers: exact dependency inventory

There are 38 overlapping header paths and exactly **24 differing copies** under `include/` and `a52-compat/include/`. In each of the repaired and NO-A3C clean builds, **3,272 object .cmd files** were examined: **125 objects** reference the differing copies, with **819 per-object-header dependency matches**. Counts below distinguish records that reference the ordinary copy only, compatibility copy only, or both. These are dependencies, not proof of which guarded declaration wins.

| Header (relative to include root) | Ordinary only | Compat only | Both |
|---|---:|---:|---:|
| `linux/clk/qcom.h` | 6 | 0 | 0 |
| `linux/input/sec_tsp_log.h` | 3 | 0 | 0 |
| `linux/ipa.h` | 49 | 0 | 0 |
| `linux/ipa_mhi.h` | 48 | 0 | 0 |
| `linux/ipa_odu_bridge.h` | 37 | 0 | 0 |
| `linux/ipa_uc_offload.h` | 48 | 0 | 0 |
| `linux/ipa_usb.h` | 37 | 0 | 0 |
| `linux/ipa_wdi3.h` | 48 | 0 | 0 |
| `linux/ipa_wigig.h` | 48 | 0 | 0 |
| `linux/ipc_logging.h` | 57 | 0 | 0 |
| `linux/mailbox/qmp.h` | 37 | 0 | 0 |
| `linux/msm-bus-board.h` | 19 | 0 | 0 |
| `linux/msm_gsi.h` | 51 | 0 | 0 |
| `net/rmnet_config.h` | 1 | 0 | 0 |
| `soc/qcom/secure_buffer.h` | 20 | 1 | 0 |
| `soc/qcom/subsystem_notif.h` | 25 | 52 | 2 |
| `soc/qcom/subsystem_restart.h` | 11 | 51 | 4 |
| `trace/events/fastrpc.h` | 1 | 0 | 0 |
| `trace/events/trace_msm_pil_event.h` | 2 | 0 | 0 |
| `uapi/linux/ipa_qmi_service_v01.h` | 49 | 0 | 0 |
| `uapi/linux/msm_ipa.h` | 49 | 0 | 0 |
| `uapi/linux/msm_rmnet.h` | 34 | 0 | 0 |
| `uapi/linux/rmnet_ipa_fd_ioctl.h` | 34 | 0 | 0 |
| `uapi/scsi/ufs/ioctl.h` | 1 | 0 | 0 |

Four compiled objects have dependencies mentioning both versions of restart/notifier headers:
- `drivers/platform/msm/ipa/ipa_v3/.ipa.o.cmd`
- `drivers/platform/msm/ipa/ipa_v3/.rmnet_ipa.o.cmd`
- `drivers/platform/msm/ipa/ipa_v3/.ipa_qmi_service.o.cmd`
- `drivers/a52_pil/.subsystem_restart.o.cmd`

The original P479 ZIP has **no original .o.cmd files**. Historical object-specific include search order is unknown from those files. However, original P479 ELF DWARF identifies the compatibility header for `subsys_desc`, size 464. Do not globally replace all 24 headers without object-level ABI inspection.

## D: top 20 data-symbol size increases, accepted by Claude

The earlier D comparison was **accepted by Claude**. It shows +4,022,436 bytes across data symbol-size entries. The largest 20 entries are:

| # | Data symbol | Increase (bytes) |
|---:|---|---:|
| 1 | `.compoundliteral` | 639,108 |
| 2 | `fe_dai_app_type_cfg` | 576,160 |
| 3 | `channel_mixer_v2` | 463,856 |
| 4 | `intercon_tdm` | 333,408 |
| 5 | `channel_mixer` | 231,928 |
| 6 | `session_copp_map` | 230,464 |
| 7 | `ec_ref_chmix_cfg` | 227,552 |
| 8 | `msm_qdsp6_widgets_tdm` | 141,760 |
| 9 | `intercon` | 139,104 |
| 10 | `msm_qdsp6_widgets` | 101,440 |
| 11 | `intercon_mi2s` | 66,144 |
| 12 | `msm_dai_q6_tdm_dai` | 35,840 |
| 13 | `msm_fe_dais` | 32,928 |
| 14 | `msm_qdsp6_widgets_mi2s` | 30,400 |
| 15 | `intercon_aux_pcm` | 24,768 |
| 16 | `msm_bedais` | 24,376 |
| 17 | `msm_kona_dai_links` | 21,840 |
| 18 | `tdm_config_controls_header_type` | 12,800 |
| 19 | `tdm_config_controls_header` | 12,800 |
| 20 | `tdm_config_controls_data_format` | 12,800 |

These 20 entries sum to **3,359,476 bytes**; `.compoundliteral` is an aggregate name that may combine unrelated anonymous constants. Complete D source: [run 44880139058](https://github.com/GiulianoB-1/A526B-Mangrove-Source/actions/runs/44880139058).

## P480 boot-container verification

The P480 A3C and NO-A3C review boots from [successful run 44809264636](https://github.com/GiulianoB-1/A526B-Mangrove-Source/actions/runs/44809264636) use the final original P479 boot template, not the Phase447C/WX template. Original P479 DTB SHA256 `1c5a9a3ec9e9391bedb4716996dc57e1a9c982e44f8c1c1196773fdbb364ec28`, ramdisk SHA256 `54f7891881a755536e908acd66fa2cf48bb24ed5f9f8d2fc4b0bf259f395a4c1`, and command line match in both. Only kernel size and boot-ID header fields change.

## Next safety gate

Review the **two dynamic-debug function codegen differences** (and the 1-byte vermagic change) against original P479, then use object-level include provenance for the mixed-header objects. Symbol-size equality is not binary or runtime identity. **NO FLASH, no merge, no new workflows.**

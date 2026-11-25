# P487 B2 investigation and staged diagnostics

Hardware report supplied by Claude: mobile data worked after the 204-second modem SSR on the staged build. No claim that the full TG SSR lifecycle passed.

The TG legacy subsystem_restart.c publishes BEFORE_SHUTDOWN (0), AFTER_SHUTDOWN (1), BEFORE_POWERUP (2), AFTER_POWERUP (3) around actual shutdown/powerup, via the synchronous SRCU provider. The existing native remoteproc_core.c P470 producer queues 0/1/3 but has no event 2. The worker is ordered but asynchronous relative to PAS, and producer rc is notifier-chain return, not a consumer or QMI acknowledgement. It uses "modem", matching SUBSYS_LOCAL_MODEM in ipa_qmi_service.h; CONFIG_MSM_SUBSYSTEM_RESTART, CONFIG_IPA3 and CONFIG_RMNET_IPA3 are enabled; source lagoon-earlyboot-probe.dtsi includes qcom,rmnet-ipa-ssr. The live merged boot DT still requires verification.

rmnet_ipa.c callback handles all four event types, stops network queue and performs Q6/QMI cleanup on 0, remaining post-shutdown cleanup on 1, QMI service exit/pre-powerup cleanup and proxy vote on 2, and conditional driver re-register/ODL open on 3. It returns NOTIFY_DONE; no fake success. QMI INIT_MODEM uses actual ipa3_uc_loaded_check() and sets is_ssr_bootup=1 on already-loaded uC (TG logic). Do not hardcode the second request or emit a pretend handshake.

Do not add event 2 alone or flush the ordered queue while holding rproc->lock. Existing callbacks can synchronously tear down/reprobe devices, and legacy subsystem wrappers own rproc power references; this needs serialization/lock and consumer-lifetime review. Existing P469 crashloop gate, legacy notification suppression and PAS recovery unchanged in this commit.

Implemented bounded full SMEM SFR logging in qcom_q6v5.c (240-byte chunks with actual total/offset on SM6350 MSS); no firmware memory writes. Added root-only one-shot software watchdog-report test attribute, hidden by default and only visible for actual SM6350 MSS with a52.mss_ssr_test=1. It requires RUNNING with recovery enabled; queues the existing rproc_report_crash path; marker expressly says software_watchdog_report, queued only, not a physical fatal IRQ or proof of recovery. No hardware action or test performed. Do not activate before Claude review. B2 lifecycle synchronization and second real QMI handshake still incomplete. Final P487/NOAUTO not packaged. NO FLASH.

## Review 2 synchronous subdevice candidate (default OFF)

Implemented the four hooks, attached last before rproc_add for the actual SM6350 MSS, with the legacy wrapper suppression retained. When explicitly selected via a52.mss_sync_ssr=1 and successfully attached, the old async producer is suppressed for that instance. When not selected, the prior behaviour remains. No completion is fabricated for an unprepare with no stop hook. Actual QMI checked response marker joins the existing real request flag and result. The gate must not be enabled for hardware testing until the complete deadlock audit passes. This is not the final B2 implementation and not a claim that callbacks are safe under rproc->lock.

Consumer inventory from source search:
- rmnet_ipa: ipa3_lcl_mdm_ssr_notifier_cb -> Q6/IPA cleanup, QMI stop/service exit, wan ioctl, stats cancellation, platform_driver_unregister/register, GSI/ODL/MHI proxy. rmnet remove calls cancel_work_sync(ipa3_tx_wakequeue_work); stats polling cancels delayed work synchronously. No direct rproc_* calls found in inspected IPA rmnet, QMI, MHI proxy, ODL or ioctl files. Transitive work dependencies and GSI teardown still not exhaustively proved.
- glink_probe: glink_ssr_ssr_cb waits up to HZ for remote cleanup response under ssr_lock; glink_probe_ssr_cb registers/unregisters edges. Need prove no dependency on same MSS lock or unavailable transport; glink_ssr_remove schedules async notifier unregister. Live DT registration identity and duplicate edge ownership unverified.
- microdump_modem_notifier_nb ignores events 0..3; only ramdump or SOC reset.
- qiib_restart_notifier_cb updates in_reset and wakes poll waitqueue on events 0/3; no direct rproc call. Actual DT ssr_name requires validation.
- audio_notifer_ssr_modem_cb fans out a secondary SRCU chain under notifier_mutex; child registration/callback closure still needs audit.
- IPA core subsystem_get path is for lagoon_ipa_fws PAS15; not a direct modem call, but indirect firmware and work waits need exclusion.

Thus the requested proof is not yet established. Do not use the opt-in candidate or claim B2 pass before review. Main unchanged, NO FLASH.

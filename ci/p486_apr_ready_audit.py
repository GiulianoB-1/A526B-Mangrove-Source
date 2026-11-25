#!/usr/bin/env python3
"""P486 real APR readiness contract. Static checks, not hardware proof."""
import re
from pathlib import Path


def audit_apr_card_ready(root):
    base = Path(root) / "drivers/soc/qcom/p472_tg_audio"
    diag = (base / "a3c/p481_diag.c").read_text()
    apr = (base / "ipc/apr.c").read_text()
    transport = (base / "ipc/apr_tal_rpmsg.c").read_text()

    def body(source, signature):
        # All these functions use a top-level closing brace on its own line.
        return source.split(signature, 1)[1].split("\n}", 1)[0]

    def require(pattern, source, label):
        if not re.search(pattern, source, re.S):
            raise AssertionError("P486 APR card gate: " + label)

    worker = body(diag, "static void p483_auto_work(struct work_struct *work)\n{")
    require(r"if \(!READ_ONCE\(card_enable\) \|\| READ_ONCE\(card_attempted\)\)\s*return;.*?if \(apr_get_q6_state\(\) != APR_SUBSYS_LOADED\)\s*return;.*?p483_register_card\(\);",
            worker, "worker rejects disabled, attempted or not-ready card")
    register = body(diag, "static int p483_register_card(void)\n{")
    require(r"mutex_lock\(&p481_lock\);.*?if \(card_attempted\) \{.*?return -EBUSY;.*?if \(!card_enable\) \{.*?return -ECANCELED;.*?WRITE_ONCE\(card_attempted, true\);.*?mutex_unlock\(&p481_lock\);.*?p480_kona_machine_init\(\);",
            register, "locked one-shot registration gate")
    hook = body(diag, "void p486_card_adsp_up(void)\n{")
    require(r"if \(READ_ONCE\(p486_auto_started\) && READ_ONCE\(card_enable\) &&\s*!READ_ONCE\(card_attempted\)\)\s*schedule_work\(&p483_card_job\);",
            hook, "UP queues work only after initialization")
    startup = body(diag, "void p483_card_auto_start(void)\n{")
    require(r"WRITE_ONCE\(p486_auto_started, true\);\s*smp_mb\(\);\s*if \(apr_get_q6_state\(\) == APR_SUBSYS_LOADED\)\s*p486_card_adsp_up\(\);",
            startup, "already-loaded startup and event race gate")
    manual = body(diag, "static ssize_t card_enable_store(")
    require(r"if \(val\)\s*p486_card_adsp_up\(\);\s*else\s*cancel_work_sync\(&p483_card_job\);",
            manual, "manual enable and cancellation")
    setter = body(apr, "int apr_set_q6_state(")
    require(r"if \(state == APR_SUBSYS_LOADED &&\s*!atomic_read\(&p472_tg_a0b_ready\)\) \{.*?return -EAGAIN;.*?atomic_set\(&q6.q6_state, state\);",
            setter, "APR LOADED requires real readiness")
    ready = body(apr, "void p472_tg_apr_a0b_ready(void)\n{")
    require(r"atomic_set\(&p472_tg_a0b_ready, 1\);.*?apr_adsp_up\(\);",
            ready, "transport readiness reaches APR")
    up = body(apr, "static void apr_adsp_up(void)\n{")
    require(r"if \(!atomic_read\(&p472_tg_a0b_ready\)\) \{.*?return;.*?apr_set_q6_state\(APR_SUBSYS_LOADED\);.*?snd_event_notify\(apr_priv->dev, SND_EVENT_UP\);\s*#ifdef CONFIG_P480_A3C.*?smp_mb\(\);\s*p486_card_adsp_up\(\);",
            up, "genuine UP event schedules card")
    transport_work = body(transport, "static void p472_tg_apr_ready_work(struct work_struct *work)\n{")
    require(r"cmd.opcode = P472_TG_APR_GET_STATE;.*?rpmsg_send\(.*?wait_for_completion_timeout\(.*?if \(waited && READ_ONCE\(ctx->adsp_state\)\) \{.*?p472_tg_apr_a0b_ready\(\);",
            transport_work, "real nonzero GET_STATE response")
    if "auto_checks" in diag or "q6core_is_adsp_ready()" in diag or "DECLARE_DELAYED_WORK(p483_card_job" in diag:
        raise AssertionError("P486 APR card gate: obsolete card polling returned")
    return True

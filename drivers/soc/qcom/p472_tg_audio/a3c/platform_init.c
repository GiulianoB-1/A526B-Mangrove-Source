
// SPDX-License-Identifier: GPL-2.0-only
/* P480 A3c: only real Q6 PCM/compress/routing/DAI providers.
 * P483 adds the real voice controls only; no LSM/listen, HDMI or voice/VoIP PCM
 * and no unported transcode/loopback/HDMI platform initializers.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include "platform_init.h"
#include "../include/dsp/q6voice.h"

static bool voice_platform_registered;

bool p483_voice_platform_ready(void)
{
    return voice_platform_registered;
}

/* Called only after A2a calibration and A3a Q6 service initialization. */
int __init p480_audio_platform_init(void)
{
    int rc;
    rc=msm_compress_dsp_init();
    if (rc) return rc;
    rc=msm_fe_dai_init();
    if (rc) goto err_fe;
    rc=msm_dai_q6_init();
    if (rc) goto err_q6;
    rc=msm_dai_stub_init();
    if (rc) goto err_stub;
    rc=msm_pcm_afe_init();
    if (rc) goto err_afe;
    rc=msm_pcm_hostless_init();
    if (rc) goto err_hostless;
    rc=msm_pcm_noirq_init();
    if (rc) goto err_noirq;
    rc=msm_pcm_dsp_init();
    if (rc) goto err_pcm;
    rc=msm_soc_routing_platform_init();
    if (rc) goto err_routing;
    if (is_voc_initialized()) {
        rc = msm_pcm_voice_init();
        pr_info("P483 VOICE_PLATFORM register rc=%d\n", rc);
        voice_platform_registered = !rc;
    } else {
        pr_err("P483 VOICE_PLATFORM register rc=%d\n", -ENODEV);
    }
    pr_info("P480 A3c PART PLATFORM_REGISTER probe rc=0\n");
    return 0;
err_routing:
    msm_pcm_dsp_exit();
err_pcm:
    msm_pcm_noirq_exit();
err_noirq:
    msm_pcm_hostless_exit();
err_hostless:
    msm_pcm_afe_exit();
err_afe:
    msm_dai_stub_exit();
err_stub:
    msm_dai_q6_exit();
err_q6:
    msm_fe_dai_exit();
err_fe:
    msm_compress_dsp_exit();
    pr_err("P480 A3c PART PLATFORM_REGISTER probe rc=%d\n", rc);
    return rc;
}
/* Init-only rollback if Kona machine registration cannot complete. */
void __init p480_audio_platform_exit(void)
{
    if (voice_platform_registered) {
        msm_pcm_voice_exit();
        voice_platform_registered = false;
    }
    msm_soc_routing_platform_exit();
    msm_pcm_dsp_exit();
    msm_pcm_noirq_exit();
    msm_pcm_hostless_exit();
    msm_pcm_afe_exit();
    msm_dai_stub_exit();
    msm_dai_q6_exit();
    msm_fe_dai_exit();
    msm_compress_dsp_exit();
}
/* Built in. No independent initcall: A3a invokes this explicitly. */
MODULE_DESCRIPTION("P483 native Q6 ASoC providers (voice controls only)");
MODULE_LICENSE("GPL v2");

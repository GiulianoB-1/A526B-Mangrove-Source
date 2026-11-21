
// SPDX-License-Identifier: GPL-2.0-only
/* P480 A3c: only real Q6 PCM/compress/routing/DAI providers.
 * In particular: no LSM/listen, no HDMI, no voice/VoIP/CVD
 * and no unported transcode/loopback/HDMI platform initializers.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include "platform_init.h"

static int __init p480_audio_platform_init(void)
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
static void __exit p480_audio_platform_exit(void)
{
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
module_init(p480_audio_platform_init);
module_exit(p480_audio_platform_exit);
MODULE_DESCRIPTION("P480 native Q6 ASoC providers (no voice)");
MODULE_LICENSE("GPL v2");

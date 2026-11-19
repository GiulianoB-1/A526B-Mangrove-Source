/* SPDX-License-Identifier: GPL-2.0-only */
/* P472 A3a: Q6ASM-only compatibility with pinned TG 4.19 compressed ABI.
 * Do not alter GKI's global include/uapi/sound/compress_params.h.
 * Source: TG 6bf351bd include/uapi/sound/compress_params.h.
 */
#ifndef P472_A3A_Q6ASM_COMPRESS_COMPAT_H
#define P472_A3A_Q6ASM_COMPRESS_COMPAT_H

#include <linux/types.h>
#include <linux/build_bug.h>

/* Preserve TouchGrass codec IDs sent to the userspace event callback. */
#define SND_AUDIOCODEC_AC3    ((__u32) 0x00000015)
#define SND_AUDIOCODEC_DTS    ((__u32) 0x00000018)
#define SND_AUDIOCODEC_EAC3   ((__u32) 0x00000019)
#define SND_AUDIOCODEC_TRUEHD ((__u32) 0x00000024)

/* Preserve the 32-byte metadata prefix used for physical buffer mapping. */
#define COMPRESSED_TIMESTAMP_FLAG 0x0001
struct snd_codec_metadata {
    __u32 length;
    __u32 offset;
    __u64 timestamp;
    __u32 reserved[4];
};
static_assert(sizeof(struct snd_codec_metadata) == 32);

#endif

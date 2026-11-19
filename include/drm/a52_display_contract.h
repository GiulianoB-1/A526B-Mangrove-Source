#ifndef __A52_DISPLAY_CONTRACT_H__
#define __A52_DISPLAY_CONTRACT_H__
#include <linux/types.h>
#include <drm/drm_bridge.h>
#include <uapi/drm/msm_drm.h>
#include <uapi/drm/sde_drm.h>

enum fps {
	FPS0 = 0,
	FPS30 = 30,
	FPS48 = 48,
	FPS60 = 60,
	FPS90 = 90,
	FPS120 = 120,
	FPS144 = 144,
};

#endif

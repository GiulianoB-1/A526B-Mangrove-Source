/* A52_PHASE446V_SDE_AOD_STATE_DIFF_V1 */
#ifndef _A52_446V_DISPLAY_TRACE_H
#define _A52_446V_DISPLAY_TRACE_H
#include <linux/types.h>
struct drm_device;
void a52v_sde_snapshot(struct drm_device *dev,const char *phase);
void a52v_atomic_snapshot(struct drm_device *dev);
void a52v_display_event(bool normal_on);
int a52v_display_stage(void);
int a52v_post_window_next(void);
#endif

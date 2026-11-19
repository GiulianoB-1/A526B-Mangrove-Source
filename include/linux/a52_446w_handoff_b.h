/* 446W-B, first HWC full modeset transaction, diagnostics */
#ifndef _A52_446W_HANDOFF_B_H
#define _A52_446W_HANDOFF_B_H
struct drm_device;
bool a52w_handoff_selected(void);
bool a52w_handoff_done(void);
void a52w_handoff_finish(struct drm_device *dev);
#endif

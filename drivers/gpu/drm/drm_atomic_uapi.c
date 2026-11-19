#include <linux/workqueue.h>
#include <linux/init.h>
#include <linux/a52_phase447.h> /* A52_PHASE447C_QUIET_PER_COMMIT_V1 */
#include <linux/ktime.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/a52_446v_display_trace.h>
#include <linux/a52_446u_trace.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
/*
 * Copyright (C) 2014 Red Hat
 * Copyright (C) 2014 Intel Corp.
 * Copyright (C) 2018 Intel Corp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 * Rob Clark <robdclark@gmail.com>
 * Daniel Vetter <daniel.vetter@ffwll.ch>
 */

#include <drm/drm_atomic_uapi.h>
#include <drm/drm_atomic.h>
#include <drm/drm_print.h>
#include <drm/drm_drv.h>
#include <drm/drm_writeback.h>
#include <drm/drm_vblank.h>

#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/dma-fence.h>
#include <linux/uaccess.h>
#include <linux/sync_file.h>
#include <linux/file.h>

#include "drm_crtc_internal.h"

/**
 * DOC: overview
 *
 * This file contains the marshalling and demarshalling glue for the atomic UAPI
 * in all its forms: The monster ATOMIC IOCTL itself, code for GET_PROPERTY and
 * SET_PROPERTY IOCTLs. Plus interface functions for compatibility helpers and
 * drivers which have special needs to construct their own atomic updates, e.g.
 * for load detect or similiar.
 */

/**
 * drm_atomic_set_mode_for_crtc - set mode for CRTC
 * @state: the CRTC whose incoming state to update
 * @mode: kernel-internal mode to use for the CRTC, or NULL to disable
 *
 * Set a mode (originating from the kernel) on the desired CRTC state and update
 * the enable property.
 *
 * RETURNS:
 * Zero on success, error code on failure. Cannot return -EDEADLK.
 */
int drm_atomic_set_mode_for_crtc(struct drm_crtc_state *state,
				 const struct drm_display_mode *mode)
{
	struct drm_crtc *crtc = state->crtc;
	struct drm_mode_modeinfo umode;

	/* Early return for no change. */
	if (mode && memcmp(&state->mode, mode, sizeof(*mode)) == 0)
		return 0;

	drm_property_blob_put(state->mode_blob);
	state->mode_blob = NULL;

	if (mode) {
		struct drm_property_blob *blob;

		drm_mode_convert_to_umode(&umode, mode);
		blob = drm_property_create_blob(crtc->dev,
						sizeof(umode), &umode);
		if (IS_ERR(blob))
			return PTR_ERR(blob);

		drm_mode_copy(&state->mode, mode);

		state->mode_blob = blob;
		state->enable = true;
		DRM_DEBUG_ATOMIC("Set [MODE:%s] for [CRTC:%d:%s] state %p\n",
				 mode->name, crtc->base.id, crtc->name, state);
	} else {
		memset(&state->mode, 0, sizeof(state->mode));
		state->enable = false;
		DRM_DEBUG_ATOMIC("Set [NOMODE] for [CRTC:%d:%s] state %p\n",
				 crtc->base.id, crtc->name, state);
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_set_mode_for_crtc);

/**
 * drm_atomic_set_mode_prop_for_crtc - set mode for CRTC
 * @state: the CRTC whose incoming state to update
 * @blob: pointer to blob property to use for mode
 *
 * Set a mode (originating from a blob property) on the desired CRTC state.
 * This function will take a reference on the blob property for the CRTC state,
 * and release the reference held on the state's existing mode property, if any
 * was set.
 *
 * RETURNS:
 * Zero on success, error code on failure. Cannot return -EDEADLK.
 */
int drm_atomic_set_mode_prop_for_crtc(struct drm_crtc_state *state,
                                      struct drm_property_blob *blob)
{
	struct drm_crtc *crtc = state->crtc;

	if (blob == state->mode_blob)
		return 0;

	drm_property_blob_put(state->mode_blob);
	state->mode_blob = NULL;

	memset(&state->mode, 0, sizeof(state->mode));

	if (blob) {
		int ret;

		if (blob->length != sizeof(struct drm_mode_modeinfo)) {
			DRM_DEBUG_ATOMIC("[CRTC:%d:%s] bad mode blob length: %zu\n",
					 crtc->base.id, crtc->name,
					 blob->length);
			return -EINVAL;
		}

		ret = drm_mode_convert_umode(crtc->dev,
					     &state->mode, blob->data);
		if (ret) {
			DRM_DEBUG_ATOMIC("[CRTC:%d:%s] invalid mode (ret=%d, status=%s):\n",
					 crtc->base.id, crtc->name,
					 ret, drm_get_mode_status_name(state->mode.status));
			drm_mode_debug_printmodeline(&state->mode);
			return -EINVAL;
		}

		state->mode_blob = drm_property_blob_get(blob);
		state->enable = true;
		DRM_DEBUG_ATOMIC("Set [MODE:%s] for [CRTC:%d:%s] state %p\n",
				 state->mode.name, crtc->base.id, crtc->name,
				 state);
	} else {
		state->enable = false;
		DRM_DEBUG_ATOMIC("Set [NOMODE] for [CRTC:%d:%s] state %p\n",
				 crtc->base.id, crtc->name, state);
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_set_mode_prop_for_crtc);

/**
 * drm_atomic_set_crtc_for_plane - set CRTC for plane
 * @plane_state: the plane whose incoming state to update
 * @crtc: CRTC to use for the plane
 *
 * Changing the assigned CRTC for a plane requires us to grab the lock and state
 * for the new CRTC, as needed. This function takes care of all these details
 * besides updating the pointer in the state object itself.
 *
 * Returns:
 * 0 on success or can fail with -EDEADLK or -ENOMEM. When the error is EDEADLK
 * then the w/w mutex code has detected a deadlock and the entire atomic
 * sequence must be restarted. All other errors are fatal.
 */
int
drm_atomic_set_crtc_for_plane(struct drm_plane_state *plane_state,
			      struct drm_crtc *crtc)
{
	struct drm_plane *plane = plane_state->plane;
	struct drm_crtc_state *crtc_state;

	if (ktime_get_boottime_ns() >= 85ULL * 1000000000ULL &&
	    plane_state && plane_state->plane &&
	    plane_state->plane->base.id == 91)
		pr_info("P446U SET_CRTC seq=%d plane=91 fb=%u old_crtc=%u new_crtc=%u caller=%pS\n",
			a52_446u_request_id(),
			plane_state->fb ? plane_state->fb->base.id : 0,
			plane_state->crtc ? plane_state->crtc->base.id : 0,
			crtc ? crtc->base.id : 0,
			__builtin_return_address(0));

	/* Nothing to do for same crtc*/
	if (plane_state->crtc == crtc)
		return 0;
	if (plane_state->crtc) {
		crtc_state = drm_atomic_get_crtc_state(plane_state->state,
						       plane_state->crtc);
		if (WARN_ON(IS_ERR(crtc_state)))
			return PTR_ERR(crtc_state);

		crtc_state->plane_mask &= ~drm_plane_mask(plane);
	}

	plane_state->crtc = crtc;

	if (crtc) {
		crtc_state = drm_atomic_get_crtc_state(plane_state->state,
						       crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);
		crtc_state->plane_mask |= drm_plane_mask(plane);
	}

	if (crtc)
		DRM_DEBUG_ATOMIC("Link [PLANE:%d:%s] state %p to [CRTC:%d:%s]\n",
				 plane->base.id, plane->name, plane_state,
				 crtc->base.id, crtc->name);
	else
		DRM_DEBUG_ATOMIC("Link [PLANE:%d:%s] state %p to [NOCRTC]\n",
				 plane->base.id, plane->name, plane_state);

	return 0;
}
EXPORT_SYMBOL(drm_atomic_set_crtc_for_plane);

/**
 * drm_atomic_set_fb_for_plane - set framebuffer for plane
 * @plane_state: atomic state object for the plane
 * @fb: fb to use for the plane
 *
 * Changing the assigned framebuffer for a plane requires us to grab a reference
 * to the new fb and drop the reference to the old fb, if there is one. This
 * function takes care of all these details besides updating the pointer in the
 * state object itself.
 */
void
drm_atomic_set_fb_for_plane(struct drm_plane_state *plane_state,
			    struct drm_framebuffer *fb)
{
	struct drm_plane *plane = plane_state->plane;

	if (ktime_get_boottime_ns() >= 85ULL * 1000000000ULL &&
	    plane_state && plane_state->plane &&
	    plane_state->plane->base.id == 91)
		if (a52_447c_trace_verbose()) pr_info("P446U SET_FB seq=%d plane=91 old_fb=%u new_fb=%u crtc=%u caller=%pS\n",
			a52_446u_request_id(),
			plane_state->fb ? plane_state->fb->base.id : 0,
			fb ? fb->base.id : 0,
			plane_state->crtc ? plane_state->crtc->base.id : 0,
			__builtin_return_address(0));


	if (fb)
		DRM_DEBUG_ATOMIC("Set [FB:%d] for [PLANE:%d:%s] state %p\n",
				 fb->base.id, plane->base.id, plane->name,
				 plane_state);
	else
		DRM_DEBUG_ATOMIC("Set [NOFB] for [PLANE:%d:%s] state %p\n",
				 plane->base.id, plane->name, plane_state);

	drm_framebuffer_assign(&plane_state->fb, fb);
}
EXPORT_SYMBOL(drm_atomic_set_fb_for_plane);

/**
 * drm_atomic_set_fence_for_plane - set fence for plane
 * @plane_state: atomic state object for the plane
 * @fence: dma_fence to use for the plane
 *
 * Helper to setup the plane_state fence in case it is not set yet.
 * By using this drivers doesn't need to worry if the user choose
 * implicit or explicit fencing.
 *
 * This function will not set the fence to the state if it was set
 * via explicit fencing interfaces on the atomic ioctl. In that case it will
 * drop the reference to the fence as we are not storing it anywhere.
 * Otherwise, if &drm_plane_state.fence is not set this function we just set it
 * with the received implicit fence. In both cases this function consumes a
 * reference for @fence.
 *
 * This way explicit fencing can be used to overrule implicit fencing, which is
 * important to make explicit fencing use-cases work: One example is using one
 * buffer for 2 screens with different refresh rates. Implicit fencing will
 * clamp rendering to the refresh rate of the slower screen, whereas explicit
 * fence allows 2 independent render and display loops on a single buffer. If a
 * driver allows obeys both implicit and explicit fences for plane updates, then
 * it will break all the benefits of explicit fencing.
 */
void
drm_atomic_set_fence_for_plane(struct drm_plane_state *plane_state,
			       struct dma_fence *fence)
{
	if (plane_state->fence) {
		dma_fence_put(fence);
		return;
	}

	plane_state->fence = fence;
}
EXPORT_SYMBOL(drm_atomic_set_fence_for_plane);

/**
 * drm_atomic_set_crtc_for_connector - set CRTC for connector
 * @conn_state: atomic state object for the connector
 * @crtc: CRTC to use for the connector
 *
 * Changing the assigned CRTC for a connector requires us to grab the lock and
 * state for the new CRTC, as needed. This function takes care of all these
 * details besides updating the pointer in the state object itself.
 *
 * Returns:
 * 0 on success or can fail with -EDEADLK or -ENOMEM. When the error is EDEADLK
 * then the w/w mutex code has detected a deadlock and the entire atomic
 * sequence must be restarted. All other errors are fatal.
 */
int
drm_atomic_set_crtc_for_connector(struct drm_connector_state *conn_state,
				  struct drm_crtc *crtc)
{
	struct drm_connector *connector = conn_state->connector;
	struct drm_crtc_state *crtc_state;

	if (conn_state->crtc == crtc)
		return 0;

	if (conn_state->crtc) {
		crtc_state = drm_atomic_get_new_crtc_state(conn_state->state,
							   conn_state->crtc);

		crtc_state->connector_mask &=
			~drm_connector_mask(conn_state->connector);

		drm_connector_put(conn_state->connector);
		conn_state->crtc = NULL;
	}

	if (crtc) {
		crtc_state = drm_atomic_get_crtc_state(conn_state->state, crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		crtc_state->connector_mask |=
			drm_connector_mask(conn_state->connector);

		drm_connector_get(conn_state->connector);
		conn_state->crtc = crtc;

		DRM_DEBUG_ATOMIC("Link [CONNECTOR:%d:%s] state %p to [CRTC:%d:%s]\n",
				 connector->base.id, connector->name,
				 conn_state, crtc->base.id, crtc->name);
	} else {
		DRM_DEBUG_ATOMIC("Link [CONNECTOR:%d:%s] state %p to [NOCRTC]\n",
				 connector->base.id, connector->name,
				 conn_state);
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_set_crtc_for_connector);

static void set_out_fence_for_crtc(struct drm_atomic_state *state,
				   struct drm_crtc *crtc, s32 __user *fence_ptr)
{
	state->crtcs[drm_crtc_index(crtc)].out_fence_ptr = fence_ptr;
}

static s32 __user *get_out_fence_for_crtc(struct drm_atomic_state *state,
					  struct drm_crtc *crtc)
{
	s32 __user *fence_ptr;

	fence_ptr = state->crtcs[drm_crtc_index(crtc)].out_fence_ptr;
	state->crtcs[drm_crtc_index(crtc)].out_fence_ptr = NULL;

	return fence_ptr;
}

static int set_out_fence_for_connector(struct drm_atomic_state *state,
					struct drm_connector *connector,
					s32 __user *fence_ptr)
{
	unsigned int index = drm_connector_index(connector);

	if (!fence_ptr)
		return 0;

	if (put_user(-1, fence_ptr))
		return -EFAULT;

	state->connectors[index].out_fence_ptr = fence_ptr;

	return 0;
}

static s32 __user *get_out_fence_for_connector(struct drm_atomic_state *state,
					       struct drm_connector *connector)
{
	unsigned int index = drm_connector_index(connector);
	s32 __user *fence_ptr;

	fence_ptr = state->connectors[index].out_fence_ptr;
	state->connectors[index].out_fence_ptr = NULL;

	return fence_ptr;
}

static int
drm_atomic_replace_property_blob_from_id(struct drm_device *dev,
					 struct drm_property_blob **blob,
					 uint64_t blob_id,
					 ssize_t expected_size,
					 ssize_t expected_elem_size,
					 bool *replaced)
{
	struct drm_property_blob *new_blob = NULL;

	if (blob_id != 0) {
		new_blob = drm_property_lookup_blob(dev, blob_id);
		if (new_blob == NULL)
			return -EINVAL;

		if (expected_size > 0 &&
		    new_blob->length != expected_size) {
			drm_property_blob_put(new_blob);
			return -EINVAL;
		}
		if (expected_elem_size > 0 &&
		    new_blob->length % expected_elem_size != 0) {
			drm_property_blob_put(new_blob);
			return -EINVAL;
		}
	}

	*replaced |= drm_property_replace_blob(blob, new_blob);
	drm_property_blob_put(new_blob);

	return 0;
}

static int drm_atomic_crtc_set_property(struct drm_crtc *crtc,
		struct drm_crtc_state *state, struct drm_property *property,
		uint64_t val)
{
	struct drm_device *dev = crtc->dev;
	struct drm_mode_config *config = &dev->mode_config;
	bool replaced = false;
	int ret;

	if (property == config->prop_active)
		state->active = val;
	else if (property == config->prop_mode_id) {
		struct drm_property_blob *mode =
			drm_property_lookup_blob(dev, val);
		ret = drm_atomic_set_mode_prop_for_crtc(state, mode);
		drm_property_blob_put(mode);
		return ret;
	} else if (property == config->prop_vrr_enabled) {
		state->vrr_enabled = val;
	} else if (property == config->degamma_lut_property) {
		ret = drm_atomic_replace_property_blob_from_id(dev,
					&state->degamma_lut,
					val,
					-1, sizeof(struct drm_color_lut),
					&replaced);
		state->color_mgmt_changed |= replaced;
		return ret;
	} else if (property == config->ctm_property) {
		ret = drm_atomic_replace_property_blob_from_id(dev,
					&state->ctm,
					val,
					sizeof(struct drm_color_ctm), -1,
					&replaced);
		state->color_mgmt_changed |= replaced;
		return ret;
	} else if (property == config->gamma_lut_property) {
		ret = drm_atomic_replace_property_blob_from_id(dev,
					&state->gamma_lut,
					val,
					-1, sizeof(struct drm_color_lut),
					&replaced);
		state->color_mgmt_changed |= replaced;
		return ret;
	} else if (property == config->prop_out_fence_ptr) {
		s32 __user *fence_ptr = u64_to_user_ptr(val);

		if (!fence_ptr)
			return 0;

		if (put_user(-1, fence_ptr))
			return -EFAULT;

		set_out_fence_for_crtc(state->state, crtc, fence_ptr);
	} else if (crtc->funcs->atomic_set_property) {
		return crtc->funcs->atomic_set_property(crtc, state, property, val);
	} else {
		DRM_DEBUG_ATOMIC("[CRTC:%d:%s] unknown property [PROP:%d:%s]]\n",
				 crtc->base.id, crtc->name,
				 property->base.id, property->name);
		return -EINVAL;
	}

	return 0;
}

static int
drm_atomic_crtc_get_property(struct drm_crtc *crtc,
		const struct drm_crtc_state *state,
		struct drm_property *property, uint64_t *val)
{
	struct drm_device *dev = crtc->dev;
	struct drm_mode_config *config = &dev->mode_config;

	if (property == config->prop_active)
		*val = drm_atomic_crtc_effectively_active(state);
	else if (property == config->prop_mode_id)
		*val = (state->mode_blob) ? state->mode_blob->base.id : 0;
	else if (property == config->prop_vrr_enabled)
		*val = state->vrr_enabled;
	else if (property == config->degamma_lut_property)
		*val = (state->degamma_lut) ? state->degamma_lut->base.id : 0;
	else if (property == config->ctm_property)
		*val = (state->ctm) ? state->ctm->base.id : 0;
	else if (property == config->gamma_lut_property)
		*val = (state->gamma_lut) ? state->gamma_lut->base.id : 0;
	else if (property == config->prop_out_fence_ptr)
		*val = 0;
	else if (crtc->funcs->atomic_get_property)
		return crtc->funcs->atomic_get_property(crtc, state, property, val);
	else
		return -EINVAL;

	return 0;
}

static int drm_atomic_plane_set_property(struct drm_plane *plane,
		struct drm_plane_state *state, struct drm_file *file_priv,
		struct drm_property *property, uint64_t val)
{
	struct drm_device *dev = plane->dev;
	struct drm_mode_config *config = &dev->mode_config;
	bool replaced = false;
	int ret;

	if (property == config->prop_fb_id) {
		struct drm_framebuffer *fb;

		fb = drm_framebuffer_lookup(dev, file_priv, val);
		drm_atomic_set_fb_for_plane(state, fb);
		if (fb)
			drm_framebuffer_put(fb);
	} else if (property == config->prop_in_fence_fd) {
		if (state->fence)
			return -EINVAL;

		if (U642I64(val) == -1)
			return 0;

		state->fence = sync_file_get_fence(val);
		if (!state->fence)
			return -EINVAL;

	} else if (property == config->prop_crtc_id) {
		struct drm_crtc *crtc = drm_crtc_find(dev, file_priv, val);

		if (val && !crtc)
			return -EACCES;
		return drm_atomic_set_crtc_for_plane(state, crtc);
	} else if (property == config->prop_crtc_x) {
		state->crtc_x = U642I64(val);
	} else if (property == config->prop_crtc_y) {
		state->crtc_y = U642I64(val);
	} else if (property == config->prop_crtc_w) {
		state->crtc_w = val;
	} else if (property == config->prop_crtc_h) {
		state->crtc_h = val;
	} else if (property == config->prop_src_x) {
		state->src_x = val;
	} else if (property == config->prop_src_y) {
		state->src_y = val;
	} else if (property == config->prop_src_w) {
		state->src_w = val;
	} else if (property == config->prop_src_h) {
		state->src_h = val;
	} else if (property == plane->alpha_property) {
		state->alpha = val;
	} else if (property == plane->blend_mode_property) {
		state->pixel_blend_mode = val;
	} else if (property == plane->rotation_property) {
		if (!is_power_of_2(val & DRM_MODE_ROTATE_MASK)) {
			DRM_DEBUG_ATOMIC("[PLANE:%d:%s] bad rotation bitmask: 0x%llx\n",
					 plane->base.id, plane->name, val);
			return -EINVAL;
		}
		state->rotation = val;
	} else if (property == plane->zpos_property) {
		state->zpos = val;
	} else if (property == plane->color_encoding_property) {
		state->color_encoding = val;
	} else if (property == plane->color_range_property) {
		state->color_range = val;
	} else if (property == config->prop_fb_damage_clips) {
		ret = drm_atomic_replace_property_blob_from_id(dev,
					&state->fb_damage_clips,
					val,
					-1,
					sizeof(struct drm_mode_rect),
					&replaced);
		return ret;
	} else if (plane->funcs->atomic_set_property) {
		return plane->funcs->atomic_set_property(plane, state,
				property, val);
	} else {
		DRM_DEBUG_ATOMIC("[PLANE:%d:%s] unknown property [PROP:%d:%s]]\n",
				 plane->base.id, plane->name,
				 property->base.id, property->name);
		return -EINVAL;
	}

	return 0;
}

static int
drm_atomic_plane_get_property(struct drm_plane *plane,
		const struct drm_plane_state *state,
		struct drm_property *property, uint64_t *val)
{
	struct drm_device *dev = plane->dev;
	struct drm_mode_config *config = &dev->mode_config;

	if (property == config->prop_fb_id) {
		*val = (state->fb) ? state->fb->base.id : 0;
	} else if (property == config->prop_in_fence_fd) {
		*val = -1;
	} else if (property == config->prop_crtc_id) {
		*val = (state->crtc) ? state->crtc->base.id : 0;
	} else if (property == config->prop_crtc_x) {
		*val = I642U64(state->crtc_x);
	} else if (property == config->prop_crtc_y) {
		*val = I642U64(state->crtc_y);
	} else if (property == config->prop_crtc_w) {
		*val = state->crtc_w;
	} else if (property == config->prop_crtc_h) {
		*val = state->crtc_h;
	} else if (property == config->prop_src_x) {
		*val = state->src_x;
	} else if (property == config->prop_src_y) {
		*val = state->src_y;
	} else if (property == config->prop_src_w) {
		*val = state->src_w;
	} else if (property == config->prop_src_h) {
		*val = state->src_h;
	} else if (property == plane->alpha_property) {
		*val = state->alpha;
	} else if (property == plane->blend_mode_property) {
		*val = state->pixel_blend_mode;
	} else if (property == plane->rotation_property) {
		*val = state->rotation;
	} else if (property == plane->zpos_property) {
		*val = state->zpos;
	} else if (property == plane->color_encoding_property) {
		*val = state->color_encoding;
	} else if (property == plane->color_range_property) {
		*val = state->color_range;
	} else if (property == config->prop_fb_damage_clips) {
		*val = (state->fb_damage_clips) ?
			state->fb_damage_clips->base.id : 0;
	} else if (plane->funcs->atomic_get_property) {
		return plane->funcs->atomic_get_property(plane, state, property, val);
	} else {
		return -EINVAL;
	}

	return 0;
}

static int drm_atomic_set_writeback_fb_for_connector(
		struct drm_connector_state *conn_state,
		struct drm_framebuffer *fb)
{
	int ret;

	ret = drm_writeback_set_fb(conn_state, fb);
	if (ret < 0)
		return ret;

	if (fb)
		DRM_DEBUG_ATOMIC("Set [FB:%d] for connector state %p\n",
				 fb->base.id, conn_state);
	else
		DRM_DEBUG_ATOMIC("Set [NOFB] for connector state %p\n",
				 conn_state);

	return 0;
}

static int drm_atomic_connector_set_property(struct drm_connector *connector,
		struct drm_connector_state *state, struct drm_file *file_priv,
		struct drm_property *property, uint64_t val)
{
	struct drm_device *dev = connector->dev;
	struct drm_mode_config *config = &dev->mode_config;
	bool replaced = false;
	int ret;

	if (property == config->prop_crtc_id) {
		struct drm_crtc *crtc = drm_crtc_find(dev, file_priv, val);

		if (val && !crtc)
			return -EACCES;
		return drm_atomic_set_crtc_for_connector(state, crtc);
	} else if (property == config->dpms_property) {
		/* setting DPMS property requires special handling, which
		 * is done in legacy setprop path for us.  Disallow (for
		 * now?) atomic writes to DPMS property:
		 */
		return -EINVAL;
	} else if (property == config->tv_select_subconnector_property) {
		state->tv.subconnector = val;
	} else if (property == config->tv_left_margin_property) {
		state->tv.margins.left = val;
	} else if (property == config->tv_right_margin_property) {
		state->tv.margins.right = val;
	} else if (property == config->tv_top_margin_property) {
		state->tv.margins.top = val;
	} else if (property == config->tv_bottom_margin_property) {
		state->tv.margins.bottom = val;
	} else if (property == config->tv_mode_property) {
		state->tv.mode = val;
	} else if (property == config->tv_brightness_property) {
		state->tv.brightness = val;
	} else if (property == config->tv_contrast_property) {
		state->tv.contrast = val;
	} else if (property == config->tv_flicker_reduction_property) {
		state->tv.flicker_reduction = val;
	} else if (property == config->tv_overscan_property) {
		state->tv.overscan = val;
	} else if (property == config->tv_saturation_property) {
		state->tv.saturation = val;
	} else if (property == config->tv_hue_property) {
		state->tv.hue = val;
	} else if (property == config->link_status_property) {
		/* Never downgrade from GOOD to BAD on userspace's request here,
		 * only hw issues can do that.
		 *
		 * For an atomic property the userspace doesn't need to be able
		 * to understand all the properties, but needs to be able to
		 * restore the state it wants on VT switch. So if the userspace
		 * tries to change the link_status from GOOD to BAD, driver
		 * silently rejects it and returns a 0. This prevents userspace
		 * from accidently breaking  the display when it restores the
		 * state.
		 */
		if (state->link_status != DRM_LINK_STATUS_GOOD)
			state->link_status = val;
	} else if (property == config->hdr_output_metadata_property) {
		ret = drm_atomic_replace_property_blob_from_id(dev,
				&state->hdr_output_metadata,
				val,
				sizeof(struct hdr_output_metadata), -1,
				&replaced);
		return ret;
	} else if (property == config->aspect_ratio_property) {
		state->picture_aspect_ratio = val;
	} else if (property == config->content_type_property) {
		state->content_type = val;
	} else if (property == connector->scaling_mode_property) {
		state->scaling_mode = val;
	} else if (property == config->content_protection_property) {
		if (val == DRM_MODE_CONTENT_PROTECTION_ENABLED) {
			DRM_DEBUG_KMS("only drivers can set CP Enabled\n");
			return -EINVAL;
		}
		state->content_protection = val;
	} else if (property == config->hdcp_content_type_property) {
		state->hdcp_content_type = val;
	} else if (property == connector->colorspace_property) {
		state->colorspace = val;
	} else if (property == config->writeback_fb_id_property) {
		struct drm_framebuffer *fb;
		int ret;

		fb = drm_framebuffer_lookup(dev, file_priv, val);
		ret = drm_atomic_set_writeback_fb_for_connector(state, fb);
		if (fb)
			drm_framebuffer_put(fb);
		return ret;
	} else if (property == config->writeback_out_fence_ptr_property) {
		s32 __user *fence_ptr = u64_to_user_ptr(val);

		return set_out_fence_for_connector(state->state, connector,
						   fence_ptr);
	} else if (property == connector->max_bpc_property) {
		state->max_requested_bpc = val;
	} else if (connector->funcs->atomic_set_property) {
		return connector->funcs->atomic_set_property(connector,
				state, property, val);
	} else {
		DRM_DEBUG_ATOMIC("[CONNECTOR:%d:%s] unknown property [PROP:%d:%s]]\n",
				 connector->base.id, connector->name,
				 property->base.id, property->name);
		return -EINVAL;
	}

	return 0;
}

static int
drm_atomic_connector_get_property(struct drm_connector *connector,
		const struct drm_connector_state *state,
		struct drm_property *property, uint64_t *val)
{
	struct drm_device *dev = connector->dev;
	struct drm_mode_config *config = &dev->mode_config;

	if (property == config->prop_crtc_id) {
		*val = (state->crtc) ? state->crtc->base.id : 0;
	} else if (property == config->dpms_property) {
		if (state->crtc && state->crtc->state->self_refresh_active)
			*val = DRM_MODE_DPMS_ON;
		else
			*val = connector->dpms;
	} else if (property == config->tv_select_subconnector_property) {
		*val = state->tv.subconnector;
	} else if (property == config->tv_left_margin_property) {
		*val = state->tv.margins.left;
	} else if (property == config->tv_right_margin_property) {
		*val = state->tv.margins.right;
	} else if (property == config->tv_top_margin_property) {
		*val = state->tv.margins.top;
	} else if (property == config->tv_bottom_margin_property) {
		*val = state->tv.margins.bottom;
	} else if (property == config->tv_mode_property) {
		*val = state->tv.mode;
	} else if (property == config->tv_brightness_property) {
		*val = state->tv.brightness;
	} else if (property == config->tv_contrast_property) {
		*val = state->tv.contrast;
	} else if (property == config->tv_flicker_reduction_property) {
		*val = state->tv.flicker_reduction;
	} else if (property == config->tv_overscan_property) {
		*val = state->tv.overscan;
	} else if (property == config->tv_saturation_property) {
		*val = state->tv.saturation;
	} else if (property == config->tv_hue_property) {
		*val = state->tv.hue;
	} else if (property == config->link_status_property) {
		*val = state->link_status;
	} else if (property == config->aspect_ratio_property) {
		*val = state->picture_aspect_ratio;
	} else if (property == config->content_type_property) {
		*val = state->content_type;
	} else if (property == connector->colorspace_property) {
		*val = state->colorspace;
	} else if (property == connector->scaling_mode_property) {
		*val = state->scaling_mode;
	} else if (property == config->hdr_output_metadata_property) {
		*val = state->hdr_output_metadata ?
			state->hdr_output_metadata->base.id : 0;
	} else if (property == config->content_protection_property) {
		*val = state->content_protection;
	} else if (property == config->hdcp_content_type_property) {
		*val = state->hdcp_content_type;
	} else if (property == config->writeback_fb_id_property) {
		/* Writeback framebuffer is one-shot, write and forget */
		*val = 0;
	} else if (property == config->writeback_out_fence_ptr_property) {
		*val = 0;
	} else if (property == connector->max_bpc_property) {
		*val = state->max_requested_bpc;
	} else if (connector->funcs->atomic_get_property) {
		return connector->funcs->atomic_get_property(connector,
				state, property, val);
	} else {
		return -EINVAL;
	}

	return 0;
}

int drm_atomic_get_property(struct drm_mode_object *obj,
		struct drm_property *property, uint64_t *val)
{
	struct drm_device *dev = property->dev;
	int ret;

	switch (obj->type) {
	case DRM_MODE_OBJECT_CONNECTOR: {
		struct drm_connector *connector = obj_to_connector(obj);

		WARN_ON(!drm_modeset_is_locked(&dev->mode_config.connection_mutex));
		ret = drm_atomic_connector_get_property(connector,
				connector->state, property, val);
		break;
	}
	case DRM_MODE_OBJECT_CRTC: {
		struct drm_crtc *crtc = obj_to_crtc(obj);

		WARN_ON(!drm_modeset_is_locked(&crtc->mutex));
		ret = drm_atomic_crtc_get_property(crtc,
				crtc->state, property, val);
		break;
	}
	case DRM_MODE_OBJECT_PLANE: {
		struct drm_plane *plane = obj_to_plane(obj);

		WARN_ON(!drm_modeset_is_locked(&plane->mutex));
		ret = drm_atomic_plane_get_property(plane,
				plane->state, property, val);
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/*
 * The big monster ioctl
 */

static struct drm_pending_vblank_event *create_vblank_event(
		struct drm_crtc *crtc, uint64_t user_data)
{
	struct drm_pending_vblank_event *e = NULL;

	e = kzalloc(sizeof *e, GFP_KERNEL);
	if (!e)
		return NULL;

	e->event.base.type = DRM_EVENT_FLIP_COMPLETE;
	e->event.base.length = sizeof(e->event);
	e->event.vbl.crtc_id = crtc->base.id;
	e->event.vbl.user_data = user_data;

	return e;
}

int drm_atomic_connector_commit_dpms(struct drm_atomic_state *state,
				     struct drm_connector *connector,
				     int mode)
{
	struct drm_connector *tmp_connector;
	struct drm_connector_state *new_conn_state;
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	int i, ret, old_mode = connector->dpms;
	bool active = false;

	ret = drm_modeset_lock(&state->dev->mode_config.connection_mutex,
			       state->acquire_ctx);
	if (ret)
		return ret;

	if (mode != DRM_MODE_DPMS_ON)
		mode = DRM_MODE_DPMS_OFF;

	if (connector->dpms == mode)
		goto out;

	connector->dpms = mode;

	crtc = connector->state->crtc;
	if (!crtc)
		goto out;
	ret = drm_atomic_add_affected_connectors(state, crtc);
	if (ret)
		goto out;

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto out;
	}

	for_each_new_connector_in_state(state, tmp_connector, new_conn_state, i) {
		if (new_conn_state->crtc != crtc)
			continue;
		if (tmp_connector->dpms == DRM_MODE_DPMS_ON) {
			active = true;
			break;
		}
	}

	crtc_state->active = active;
	ret = drm_atomic_commit(state);
out:
	if (ret != 0)
		connector->dpms = old_mode;
	return ret;
}

void a52ab_trace_set(struct drm_mode_object *obj,
                    struct drm_property *prop, u64 val);
int drm_atomic_set_property(struct drm_atomic_state *state,
			    struct drm_file *file_priv,
			    struct drm_mode_object *obj,
			    struct drm_property *prop,
			    uint64_t prop_value)
{
	struct drm_mode_object *ref;
	int ret;

	a52ab_trace_set(obj,prop,prop_value);
	if (!drm_property_change_valid_get(prop, prop_value, &ref))
		return -EINVAL;

	switch (obj->type) {
	case DRM_MODE_OBJECT_CONNECTOR: {
		struct drm_connector *connector = obj_to_connector(obj);
		struct drm_connector_state *connector_state;

		connector_state = drm_atomic_get_connector_state(state, connector);
		if (IS_ERR(connector_state)) {
			ret = PTR_ERR(connector_state);
			break;
		}

		ret = drm_atomic_connector_set_property(connector,
				connector_state, file_priv,
				prop, prop_value);
		break;
	}
	case DRM_MODE_OBJECT_CRTC: {
		struct drm_crtc *crtc = obj_to_crtc(obj);
		struct drm_crtc_state *crtc_state;

		crtc_state = drm_atomic_get_crtc_state(state, crtc);
		if (IS_ERR(crtc_state)) {
			ret = PTR_ERR(crtc_state);
			break;
		}

		ret = drm_atomic_crtc_set_property(crtc,
				crtc_state, prop, prop_value);
		break;
	}
	case DRM_MODE_OBJECT_PLANE: {
		struct drm_plane *plane = obj_to_plane(obj);
		struct drm_plane_state *plane_state;

		plane_state = drm_atomic_get_plane_state(state, plane);
		if (IS_ERR(plane_state)) {
			ret = PTR_ERR(plane_state);
			break;
		}

		ret = drm_atomic_plane_set_property(plane,
				plane_state, file_priv,
				prop, prop_value);
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	drm_property_change_valid_put(prop, ref);
	return ret;
}

/**
 * DOC: explicit fencing properties
 *
 * Explicit fencing allows userspace to control the buffer synchronization
 * between devices. A Fence or a group of fences are transfered to/from
 * userspace using Sync File fds and there are two DRM properties for that.
 * IN_FENCE_FD on each DRM Plane to send fences to the kernel and
 * OUT_FENCE_PTR on each DRM CRTC to receive fences from the kernel.
 *
 * As a contrast, with implicit fencing the kernel keeps track of any
 * ongoing rendering, and automatically ensures that the atomic update waits
 * for any pending rendering to complete. For shared buffers represented with
 * a &struct dma_buf this is tracked in &struct dma_resv.
 * Implicit syncing is how Linux traditionally worked (e.g. DRI2/3 on X.org),
 * whereas explicit fencing is what Android wants.
 *
 * "IN_FENCE_FD”:
 *	Use this property to pass a fence that DRM should wait on before
 *	proceeding with the Atomic Commit request and show the framebuffer for
 *	the plane on the screen. The fence can be either a normal fence or a
 *	merged one, the sync_file framework will handle both cases and use a
 *	fence_array if a merged fence is received. Passing -1 here means no
 *	fences to wait on.
 *
 *	If the Atomic Commit request has the DRM_MODE_ATOMIC_TEST_ONLY flag
 *	it will only check if the Sync File is a valid one.
 *
 *	On the driver side the fence is stored on the @fence parameter of
 *	&struct drm_plane_state. Drivers which also support implicit fencing
 *	should set the implicit fence using drm_atomic_set_fence_for_plane(),
 *	to make sure there's consistent behaviour between drivers in precedence
 *	of implicit vs. explicit fencing.
 *
 * "OUT_FENCE_PTR”:
 *	Use this property to pass a file descriptor pointer to DRM. Once the
 *	Atomic Commit request call returns OUT_FENCE_PTR will be filled with
 *	the file descriptor number of a Sync File. This Sync File contains the
 *	CRTC fence that will be signaled when all framebuffers present on the
 *	Atomic Commit * request for that given CRTC are scanned out on the
 *	screen.
 *
 *	The Atomic Commit request fails if a invalid pointer is passed. If the
 *	Atomic Commit request fails for any other reason the out fence fd
 *	returned will be -1. On a Atomic Commit with the
 *	DRM_MODE_ATOMIC_TEST_ONLY flag the out fence will also be set to -1.
 *
 *	Note that out-fences don't have a special interface to drivers and are
 *	internally represented by a &struct drm_pending_vblank_event in struct
 *	&drm_crtc_state, which is also used by the nonblocking atomic commit
 *	helpers and for the DRM event handling for existing userspace.
 */

struct drm_out_fence_state {
	s32 __user *out_fence_ptr;
	struct sync_file *sync_file;
	int fd;
};

static int setup_out_fence(struct drm_out_fence_state *fence_state,
			   struct dma_fence *fence)
{
	fence_state->fd = get_unused_fd_flags(O_CLOEXEC);
	if (fence_state->fd < 0)
		return fence_state->fd;

	if (put_user(fence_state->fd, fence_state->out_fence_ptr))
		return -EFAULT;

	fence_state->sync_file = sync_file_create(fence);
	if (!fence_state->sync_file)
		return -ENOMEM;

	return 0;
}

static int prepare_signaling(struct drm_device *dev,
				  struct drm_atomic_state *state,
				  struct drm_mode_atomic *arg,
				  struct drm_file *file_priv,
				  struct drm_out_fence_state **fence_state,
				  unsigned int *num_fences)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_connector *conn;
	struct drm_connector_state *conn_state;
	int i, c = 0, ret;

	if (arg->flags & DRM_MODE_ATOMIC_TEST_ONLY)
		return 0;

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		s32 __user *fence_ptr;

		fence_ptr = get_out_fence_for_crtc(crtc_state->state, crtc);

		if (arg->flags & DRM_MODE_PAGE_FLIP_EVENT || fence_ptr) {
			struct drm_pending_vblank_event *e;

			e = create_vblank_event(crtc, arg->user_data);
			if (!e)
				return -ENOMEM;

			crtc_state->event = e;
		}

		if (arg->flags & DRM_MODE_PAGE_FLIP_EVENT) {
			struct drm_pending_vblank_event *e = crtc_state->event;

			if (!file_priv)
				continue;

			ret = drm_event_reserve_init(dev, file_priv, &e->base,
						     &e->event.base);
			if (ret) {
				kfree(e);
				crtc_state->event = NULL;
				return ret;
			}
		}

		if (fence_ptr) {
			struct dma_fence *fence;
			struct drm_out_fence_state *f;

			f = krealloc(*fence_state, sizeof(**fence_state) *
				     (*num_fences + 1), GFP_KERNEL);
			if (!f)
				return -ENOMEM;

			memset(&f[*num_fences], 0, sizeof(*f));

			f[*num_fences].out_fence_ptr = fence_ptr;
			*fence_state = f;

			fence = drm_crtc_create_fence(crtc);
			if (!fence)
				return -ENOMEM;

			ret = setup_out_fence(&f[(*num_fences)++], fence);
			if (ret) {
				dma_fence_put(fence);
				return ret;
			}

			crtc_state->event->base.fence = fence;
		}

		c++;
	}

	for_each_new_connector_in_state(state, conn, conn_state, i) {
		struct drm_writeback_connector *wb_conn;
		struct drm_out_fence_state *f;
		struct dma_fence *fence;
		s32 __user *fence_ptr;

		if (!conn_state->writeback_job)
			continue;

		fence_ptr = get_out_fence_for_connector(state, conn);
		if (!fence_ptr)
			continue;

		f = krealloc(*fence_state, sizeof(**fence_state) *
			     (*num_fences + 1), GFP_KERNEL);
		if (!f)
			return -ENOMEM;

		memset(&f[*num_fences], 0, sizeof(*f));

		f[*num_fences].out_fence_ptr = fence_ptr;
		*fence_state = f;

		wb_conn = drm_connector_to_writeback(conn);
		fence = drm_writeback_get_out_fence(wb_conn);
		if (!fence)
			return -ENOMEM;

		ret = setup_out_fence(&f[(*num_fences)++], fence);
		if (ret) {
			dma_fence_put(fence);
			return ret;
		}

		conn_state->writeback_job->out_fence = fence;
	}

	/*
	 * Having this flag means user mode pends on event which will never
	 * reach due to lack of at least one CRTC for signaling
	 */
	if (c == 0 && (arg->flags & DRM_MODE_PAGE_FLIP_EVENT))
		return -EINVAL;

	return 0;
}

static void complete_signaling(struct drm_device *dev,
			       struct drm_atomic_state *state,
			       struct drm_out_fence_state *fence_state,
			       unsigned int num_fences,
			       bool install_fds)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	int i;

	if (install_fds) {
		for (i = 0; i < num_fences; i++)
			fd_install(fence_state[i].fd,
				   fence_state[i].sync_file->file);

		kfree(fence_state);
		return;
	}

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		struct drm_pending_vblank_event *event = crtc_state->event;
		/*
		 * Free the allocated event. drm_atomic_helper_setup_commit
		 * can allocate an event too, so only free it if it's ours
		 * to prevent a double free in drm_atomic_state_clear.
		 */
		if (event && (event->base.fence || event->base.file_priv)) {
			drm_event_cancel_free(dev, &event->base);
			crtc_state->event = NULL;
		}
	}

	if (!fence_state)
		return;

	for (i = 0; i < num_fences; i++) {
		if (fence_state[i].sync_file)
			fput(fence_state[i].sync_file->file);
		if (fence_state[i].fd >= 0)
			put_unused_fd(fence_state[i].fd);

		/* If this fails log error to the user */
		if (fence_state[i].out_fence_ptr &&
		    put_user(-1, fence_state[i].out_fence_ptr))
			DRM_DEBUG_ATOMIC("Couldn't clear out_fence_ptr\n");
	}

	kfree(fence_state);
}


/* A52_PHASE446U_ATOMIC_REQUEST_PLANE91_V1
 * The ioctl thread owns its slot until the call returns. No changing
 * DRM objects, properties or commit outcomes. Slot collision is detected.
 */
static atomic_t a52u_ioctl_seq = ATOMIC_INIT(0);
struct a52u_slot { pid_t pid; int req; };
static struct a52u_slot a52u_slots[1024];
static DEFINE_SPINLOCK(a52u_slot_lock);
int a52_446u_request_id(void)
{
	unsigned long flags;
	unsigned int idx = (unsigned int)task_pid_nr(current) & 1023;
	int req = 0;
	spin_lock_irqsave(&a52u_slot_lock, flags);
	if (a52u_slots[idx].pid == task_pid_nr(current))
		req = a52u_slots[idx].req;
	spin_unlock_irqrestore(&a52u_slot_lock, flags);
	return req;
}
EXPORT_SYMBOL_GPL(a52_446u_request_id);
static void a52u_req_set(int req)
{
	unsigned long flags;
	unsigned int idx = (unsigned int)task_pid_nr(current) & 1023;
	spin_lock_irqsave(&a52u_slot_lock, flags);
	a52u_slots[idx].pid = req ? task_pid_nr(current) : 0;
	a52u_slots[idx].req = req;
	spin_unlock_irqrestore(&a52u_slot_lock, flags);
}

#include <linux/atomic.h>
#include <linux/sched.h>

/* A52_PHASE446AB_FIFO_AND_COMPOSER_PROPERTY_EPOCHS_V1
 * Read-only, keyed to the actual HWC thread. First 20 non-TEST_ONLY
 * atomic ioctls; rearm only after composer sets ACTIVE=0 then ACTIVE=1
 * on CRTC107 or DPMS=OFF then ON on connector32. No commit suppression.
 */
static atomic_t a52ab_hwc_commits = ATOMIC_INIT(0);
static atomic_t a52ab_power_phase = ATOMIC_INIT(0); /* 0=warmup,1=armed,2=off,3=on */
static atomic_t a52ab_resume_at = ATOMIC_INIT(0);
static bool a52ab_is_composer(void)
{
	return strnstr(current->comm,"HwBinder",TASK_COMM_LEN) ||
	       strnstr(current->comm,"composer",TASK_COMM_LEN) ||
	       strnstr(current->comm,"Hwc",TASK_COMM_LEN) ||
	       strnstr(current->comm,"HWC",TASK_COMM_LEN);
}
static bool a52ab_window(void)
{
	unsigned int n=atomic_read(&a52ab_hwc_commits);
	unsigned int base=atomic_read(&a52ab_resume_at);
	return n<=20 || (atomic_read(&a52ab_power_phase)==3 &&
			    n>=base && n<base+20);
}
static const char *a52ab_epoch(void)
{
	return atomic_read(&a52ab_power_phase)==3 ? "POST_POWER_CYCLE" : "BOOT";
}
static bool a52ab_target(struct drm_mode_object *obj)
{
	return obj && ((obj->id==32 && obj->type==DRM_MODE_OBJECT_CONNECTOR) ||
		       (obj->id==107 && obj->type==DRM_MODE_OBJECT_CRTC) ||
		       (obj->id==91 && obj->type==DRM_MODE_OBJECT_PLANE));
}
static void a52ab_watch_power(struct drm_mode_object *obj,
			       struct drm_property *prop, u64 val)
{
	int phase=atomic_read(&a52ab_power_phase);
	bool on=false,off=false;
	if (obj->id==107 && obj->type==DRM_MODE_OBJECT_CRTC &&
	    !strcmp(prop->name,"ACTIVE")) {
		off=!val; on=!!val;
	} else if (obj->id==32 && obj->type==DRM_MODE_OBJECT_CONNECTOR &&
		   !strcmp(prop->name,"DPMS")) {
		off=(val==DRM_MODE_DPMS_OFF);
		on=(val==DRM_MODE_DPMS_ON);
	}
	if (phase==1 && off &&
	    atomic_cmpxchg(&a52ab_power_phase,1,2)==1)
		pr_info("P446AB POWER_CYCLE_OFF obj=%u prop=%s value=%llu hwc_commit=%d\n",
			obj->id,prop->name,(unsigned long long)val,
			atomic_read(&a52ab_hwc_commits));
	if (phase==2 && on &&
	    atomic_cmpxchg(&a52ab_power_phase,2,3)==2) {
		int base=atomic_read(&a52ab_hwc_commits);
		atomic_set(&a52ab_resume_at,base);
		pr_info("P446AB POST_POWER_CYCLE_BEGIN obj=%u prop=%s value=%llu base=%d next20=1\n",
			obj->id,prop->name,(unsigned long long)val,base);
	}
}
void a52ab_trace_set(struct drm_mode_object *obj,
		    struct drm_property *prop, u64 val)
{
	if (!a52ab_is_composer() || !a52ab_target(obj))
		return;
	a52ab_watch_power(obj,prop,val); /* watch even after first 20 */
	if (a52ab_window())
		pr_info("P446AB PROP_SET epoch=%s commit=%d obj=%u type=%u prop=%s id=%u value=%llu pid=%d comm=%s\n",
			a52ab_epoch(),atomic_read(&a52ab_hwc_commits),
			obj->id,obj->type,prop->name,prop->base.id,
			(unsigned long long)val,task_pid_nr(current),current->comm);
}
void a52ab_trace_get(struct drm_mode_object *obj,
		    struct drm_property *prop, u64 val)
{
	if (a52ab_is_composer() && a52ab_target(obj) && a52ab_window())
		pr_info("P446AB PROP_GET epoch=%s commit=%d obj=%u type=%u prop=%s id=%u value=%llu pid=%d comm=%s\n",
			a52ab_epoch(),atomic_read(&a52ab_hwc_commits),
			obj->id,obj->type,prop->name,prop->base.id,
			(unsigned long long)val,task_pid_nr(current),current->comm);
}
static void a52ab_count_hwc_ioctl(struct drm_mode_atomic *arg)
{
	int n;
	if (!a52ab_is_composer() || (arg->flags & DRM_MODE_ATOMIC_TEST_ONLY))
		return;
	n=atomic_inc_return(&a52ab_hwc_commits);
	if (n==21)
		atomic_cmpxchg(&a52ab_power_phase,0,1);
	if (a52ab_window())
		pr_info("P446AB HWC_COMMIT epoch=%s seq=%d flags=%#x objects=%u pid=%d comm=%s\n",
			a52ab_epoch(),n,arg->flags,arg->count_objs,
			task_pid_nr(current),current->comm);
}


/* A52_PHASE446AC_HWC_ATOMIC_RING_V1
 * One-shot, non-destructive HWC atomic request ring.
 * Save full object + property name/ID/value for 16 prior + first blank + 16
 * subsequent atomic requests. Read frozen ring from debugfs as root.
 */
#define A52AC_RING_SLOTS 64
#define A52AC_MAX_PROPS 96
struct a52ac_prop {
	u32 object_id,object_type,prop_id;
	u64 value;
	char name[40];
};
struct a52ac_request {
	u32 seq,flags,object_count,pid;
	u32 num_props,dropped_props;
	u64 boottime_ns;
	char comm[TASK_COMM_LEN];
	struct a52ac_prop props[A52AC_MAX_PROPS];
};
static struct a52ac_request a52ac_ring[A52AC_RING_SLOTS];
static DEFINE_SPINLOCK(a52ac_ring_lock);
static u32 a52ac_trigger_seq;
static bool a52ac_seen_fb;
static bool a52ac_frozen;
static bool a52ac_hwc(void)
{
	return strnstr(current->comm,"composer",TASK_COMM_LEN) ||
	       strnstr(current->comm,"HwBinder",TASK_COMM_LEN) ||
	       strnstr(current->comm,"HWC",TASK_COMM_LEN);
}
static void a52ac_ring_start(u32 seq,const struct drm_mode_atomic *arg)
{
	struct a52ac_request *r;
	unsigned long irqflags;
	if (!a52ac_hwc() || (arg->flags & DRM_MODE_ATOMIC_TEST_ONLY))
		return;
	spin_lock_irqsave(&a52ac_ring_lock,irqflags);
	if (a52ac_frozen) goto out;
	r=&a52ac_ring[seq%A52AC_RING_SLOTS];
	memset(r,0,sizeof(*r));
	r->seq=seq;
	r->boottime_ns=ktime_get_boottime_ns();
	r->flags=arg->flags;
	r->object_count=arg->count_objs;
	r->pid=task_pid_nr(current);
	strscpy(r->comm,current->comm,sizeof(r->comm));
out:
	spin_unlock_irqrestore(&a52ac_ring_lock,irqflags);
}
static void a52ac_ring_prop(u32 seq,struct drm_mode_object *obj,
			    struct drm_property *prop,u64 value)
{
	struct a52ac_request *r;
	struct a52ac_prop *p;
	unsigned long irqflags;
	bool triggered=false;
	if (!a52ac_hwc())return;
	spin_lock_irqsave(&a52ac_ring_lock,irqflags);
	if (a52ac_frozen)goto out;
	r=&a52ac_ring[seq%A52AC_RING_SLOTS];
	if (r->seq!=seq)goto out;
	if (r->num_props<A52AC_MAX_PROPS) {
		p=&r->props[r->num_props++];
		p->object_id=obj->id;
		p->object_type=obj->type;
		p->prop_id=prop->base.id;
		p->value=value;
		strscpy(p->name,prop->name,sizeof(p->name));
	} else {
		r->dropped_props++;
	}
	if (obj->type==DRM_MODE_OBJECT_PLANE && obj->id==91 &&
	    !strcmp(prop->name,"FB_ID")) {
		if (value)a52ac_seen_fb=true;
		else if (a52ac_seen_fb && !a52ac_trigger_seq) {
			a52ac_trigger_seq=seq;
			triggered=true;
		}
	}
out:
	spin_unlock_irqrestore(&a52ac_ring_lock,irqflags);
	if (triggered)
		pr_info("P446AC HWC_RING TRIGGER seq=%u time_ns=%llu plane91_FB_ID=0 pre=16 post=16\n",
			seq,(unsigned long long)ktime_get_boottime_ns());
}
static void a52ac_ring_end(u32 seq)
{
	unsigned long irqflags;
	bool frozen=false;
	spin_lock_irqsave(&a52ac_ring_lock,irqflags);
	if (!a52ac_frozen && a52ac_trigger_seq &&
	    seq>=a52ac_trigger_seq+16) {
		a52ac_frozen=true;
		frozen=true;
	}
	spin_unlock_irqrestore(&a52ac_ring_lock,irqflags);
	if (frozen)
		pr_info("P446AC HWC_RING READY trigger=%u through=%u debugfs=/sys/kernel/debug/a52_446ac_hwc_requests\n",
			a52ac_trigger_seq,seq);
}
static int a52ac_ring_show(struct seq_file *m,void *v)
{
	u32 seq,i,start,end,trigger;
	if (!READ_ONCE(a52ac_frozen)) {
		seq_puts(m,"P446AC HWC ring not frozen: waiting for first valid->FB_ID=0 transition and 16 later commits\n");
		return 0;
	}
	trigger=READ_ONCE(a52ac_trigger_seq);
	start=trigger>16 ? trigger-16 : 1;
	end=trigger+16;
	seq_printf(m,"P446AC HWC_RING trigger=%u range=%u..%u slots=%u maxprops=%u\n",
		   trigger,start,end,A52AC_RING_SLOTS,A52AC_MAX_PROPS);
	for (seq=start;seq<=end;seq++) {
		const struct a52ac_request *r=&a52ac_ring[seq%A52AC_RING_SLOTS];
		if (r->seq!=seq) {
			seq_printf(m,"MISSING seq=%u\n",seq);
			continue;
		}
		seq_printf(m,"REQUEST seq=%u ts_ns=%llu pid=%u comm=%s flags=%#x objects=%u props=%u dropped=%u\n",
		   seq,(unsigned long long)r->boottime_ns,r->pid,r->comm,
		   r->flags,r->object_count,r->num_props,r->dropped_props);
		for (i=0;i<r->num_props;i++) {
			const struct a52ac_prop *p=&r->props[i];
			seq_printf(m,"  PROP obj=%u type=%#x prop=%s id=%u val=%llu hex=%#llx\n",
			    p->object_id,p->object_type,p->name,p->prop_id,
			    (unsigned long long)p->value,(unsigned long long)p->value);
		}
	}
	return 0;
}
static int a52ac_ring_open(struct inode *inode,struct file *file)
{
	return single_open(file,a52ac_ring_show,NULL);
}
static const struct file_operations a52ac_ring_fops = {
	.owner=THIS_MODULE,
	.open=a52ac_ring_open,
	.read=seq_read,
	.llseek=seq_lseek,
	.release=single_release,
};
static int __init a52ac_ring_init(void)
{
	debugfs_create_file("a52_446ac_hwc_requests",0400,NULL,NULL,
			    &a52ac_ring_fops);
	return 0;
}
late_initcall(a52ac_ring_init);


/* A52_PHASE447_FLICKER_RING_V1
 * HWC-only, non TEST_ONLY; last 40 + trigger + next 20 atomic requests.
 * Default ON; early param a52.flicker_ring=0 disables without rebuild.
 * The dump is queued to process context only AFTER all 20 post requests
 * completed: does not print hundreds of lines in an atomic ioctl context.
 * All objects and requested properties (not just plane91 / FB_ID) are
 * recorded, and every bounded overflow is explicitly marked (not hidden).
 * Legacy P446AC debugfs ring retained without changing its behavior.
 */
#define A52F_RING_SLOTS 64 /* 40 + 1 + 20, with three spare slots */
#define A52F_RING_PRE 40
#define A52F_RING_POST 20
#define A52F_RING_PROPS 512
#define A52F_RING_OBJECTS 128

struct a52f_prop {
	u32 obj_id, obj_type, prop_id;
	u64 value;
	char name[40];
};
struct a52f_object {
	u32 id, type, requested_props;
};
struct a52f_req {
	u32 seq, pid, flags, expected_objs;
	u32 nobj, nprop, dropped_obj, dropped_prop;
	u64 time_ns;
	int rc;
	bool done;
	char comm[TASK_COMM_LEN];
	struct a52f_object obj[A52F_RING_OBJECTS];
	struct a52f_prop prop[A52F_RING_PROPS];
};

static struct a52f_req a52f_ring[A52F_RING_SLOTS];
static DEFINE_SPINLOCK(a52f_lock);
static u32 a52f_count, a52f_trigger;
static bool a52f_frozen;
static bool a52f_enabled=true;
static void a52f_dump_fn(struct work_struct *work);
static DECLARE_WORK(a52f_dump_work,a52f_dump_fn);

static int __init a52f_ring_param(char *s)
{
	if (!s || kstrtobool(s,&a52f_enabled))
		return -EINVAL;
	return 0;
}
early_param("a52.flicker_ring",a52f_ring_param);

static u32 a52f_begin(struct drm_mode_atomic *arg)
{
	struct a52f_req *r;
	unsigned long flags;
	u32 seq=0;
	if (!READ_ONCE(a52f_enabled) ||
	    !a52ab_is_composer() ||
	    (arg->flags & DRM_MODE_ATOMIC_TEST_ONLY))
		return 0;
	spin_lock_irqsave(&a52f_lock,flags);
	if (a52f_frozen ||
	    (a52f_trigger && a52f_count>=a52f_trigger+A52F_RING_POST))
		goto out;
	seq=++a52f_count;
	r=&a52f_ring[seq%A52F_RING_SLOTS];
	memset(r,0,sizeof(*r));
	r->seq=seq;
	r->time_ns=ktime_get_boottime_ns();
	r->pid=task_pid_nr(current);
	r->flags=arg->flags;
	r->expected_objs=arg->count_objs;
	strscpy(r->comm,current->comm,sizeof(r->comm));
out:
	spin_unlock_irqrestore(&a52f_lock,flags);
	return seq;
}

/* A drm atomic -EDEADLK retry must not append objects/properties twice. */
static void a52f_retry(u32 seq)
{
	struct a52f_req *r;
	unsigned long flags;
	if (!seq)return;
	spin_lock_irqsave(&a52f_lock,flags);
	r=&a52f_ring[seq%A52F_RING_SLOTS];
	if (!a52f_frozen && r->seq==seq) {
		r->nobj=0;
		r->nprop=0;
		r->dropped_obj=0;
		r->dropped_prop=0;
		r->done=false;
	}
	spin_unlock_irqrestore(&a52f_lock,flags);
}
static void a52f_object(u32 seq,const struct drm_mode_object *obj,u32 nprops)
{
	struct a52f_req *r;
	unsigned long flags;
	if (!seq)return;
	spin_lock_irqsave(&a52f_lock,flags);
	r=&a52f_ring[seq%A52F_RING_SLOTS];
	if (!a52f_frozen && r->seq==seq) {
		if (r->nobj<A52F_RING_OBJECTS) {
			struct a52f_object *o=&r->obj[r->nobj++];
			o->id=obj->id;
			o->type=obj->type;
			o->requested_props=nprops;
		} else {
			r->dropped_obj++;
		}
	}
	spin_unlock_irqrestore(&a52f_lock,flags);
}
static void a52f_property(u32 seq,struct drm_mode_object *obj,
			  struct drm_property *prop,u64 val)
{
	struct a52f_req *r;
	unsigned long flags;
	bool trigger=false;
	if (!seq)return;
	spin_lock_irqsave(&a52f_lock,flags);
	r=&a52f_ring[seq%A52F_RING_SLOTS];
	if (!a52f_frozen && r->seq==seq) {
		if (r->nprop<A52F_RING_PROPS) {
			struct a52f_prop *p=&r->prop[r->nprop++];
			p->obj_id=obj->id;
			p->obj_type=obj->type;
			p->prop_id=prop->base.id;
			p->value=val;
			strscpy(p->name,prop->name,sizeof(p->name));
		} else {
			r->dropped_prop++;
		}
		/* FIRST compositor plane CRTC_ID=0 REQUEST, regardless of
		 * plane number. Never substitute post-commit fb state or FB_ID.
		 */
		if (!a52f_trigger && obj->type==DRM_MODE_OBJECT_PLANE &&
		    !strcmp(prop->name,"CRTC_ID") && val==0) {
			a52f_trigger=seq;
			trigger=true;
		}
	}
	spin_unlock_irqrestore(&a52f_lock,flags);
	if (trigger)
		pr_info("P447 FLICKER_RING TRIGGER seq=%u plane=%u CRTC_ID=0 pre=%d post=%d time_ns=%llu\n",
			seq,obj->id,A52F_RING_PRE,A52F_RING_POST,
			(unsigned long long)ktime_get_boottime_ns());
}
static void a52f_end(u32 seq,int rc)
{
	unsigned long flags;
	bool ready=false,complete=true;
	u32 i;
	struct a52f_req *r;
	if (!seq)return;
	spin_lock_irqsave(&a52f_lock,flags);
	r=&a52f_ring[seq%A52F_RING_SLOTS];
	if (!a52f_frozen && r->seq==seq) {
		r->rc=rc;
		if (rc!=-EDEADLK)
			r->done=true;
	}
	if (!a52f_frozen && a52f_trigger &&
	    a52f_count>=a52f_trigger+A52F_RING_POST) {
		for (i=a52f_trigger;i<=a52f_trigger+A52F_RING_POST;i++) {
			const struct a52f_req *p=
				&a52f_ring[i%A52F_RING_SLOTS];
			if (p->seq!=i || !p->done) {
				complete=false;
				break;
			}
		}
		if (complete) {
			a52f_frozen=true;
			ready=true;
		}
	}
	spin_unlock_irqrestore(&a52f_lock,flags);
	if (ready)
		schedule_work(&a52f_dump_work);
}

/* Frozen ring is immutable; never hold the spinlock while printk() runs. */
static void a52f_dump_fn(struct work_struct *work)
{
	u32 seq,i,start,trigger=READ_ONCE(a52f_trigger);
	u32 end=trigger+A52F_RING_POST,printed=0,dropped=0;
	start=trigger>A52F_RING_PRE ? trigger-A52F_RING_PRE : 1;
	pr_info("P447 FLICKER_RING DUMP_BEGIN trigger=%u range=%u..%u wanted_pre=40 wanted_post=20 slots=%u prop_cap=%u once=1\n",
		trigger,start,end,A52F_RING_SLOTS,A52F_RING_PROPS);
	for (seq=start;seq<=end;seq++) {
		const struct a52f_req *r=&a52f_ring[seq%A52F_RING_SLOTS];
		if (r->seq!=seq) {
			pr_info("P447 FLICKER_RING MISSING seq=%u\n",seq);
			continue;
		}
		printed++;
		dropped+=r->dropped_prop+r->dropped_obj;
		pr_info("P447 FLICKER_RING REQ seq=%u ns=%llu pid=%u comm=%s flags=%#x rc=%d done=%d objects=%u/%u dropped_obj=%u props=%u dropped_prop=%u phase=%s\n",
			r->seq,(unsigned long long)r->time_ns,r->pid,r->comm,
			r->flags,r->rc,r->done,r->nobj,r->expected_objs,
			r->dropped_obj,r->nprop,r->dropped_prop,
			seq==trigger?"TRIGGER":seq<trigger?"PRE":"POST");
		for (i=0;i<r->nobj;i++) {
			const struct a52f_object *o=&r->obj[i];
			pr_info("P447 FLICKER_RING OBJ seq=%u index=%u id=%u type=%#x requested_props=%u\n",
				seq,i,o->id,o->type,o->requested_props);
		}
		for (i=0;i<r->nprop;i++) {
			const struct a52f_prop *p=&r->prop[i];
			pr_info("P447 FLICKER_RING PROP seq=%u index=%u obj=%u type=%#x name=%s prop_id=%u value=%llu hex=%#llx\n",
				seq,i,p->obj_id,p->obj_type,p->name,p->prop_id,
				(unsigned long long)p->value,
				(unsigned long long)p->value);
		}
	}
	pr_info("P447 FLICKER_RING DUMP_END trigger=%u requests=%u dropped=%u once=1 status=%s\n",
		trigger,printed,dropped,dropped?"TRUNCATED":"COMPLETE");
}
static int __init a52f_init(void)
{
	pr_info("P447 FLICKER_RING ARMED enabled=%d default_on=1 trigger=first_plane_CRTC_ID_0 pre=40 post=20 one_shot=1 dump=dmesg\n",
		a52f_enabled);
	return 0;
}
late_initcall(a52f_init);

/* A52_PHASE446AA_SE8_DRM_REQUEST_TRACE_V1; first 192 ioctls, no state modification */
static atomic_t a52aa_req_no = ATOMIC_INIT(0);
int drm_mode_atomic_ioctl(struct drm_device *dev,
			  void *data, struct drm_file *file_priv)
{
	/* A52_PHASE446S_ATOMIC_IOCTL_TRACE_V1 */
	struct drm_mode_atomic *arg = data;
	unsigned int a52aa_seq=0;
	u32 a52f_seq=0;
	uint32_t __user *objs_ptr = (uint32_t __user *)(unsigned long)(arg->objs_ptr);
	uint32_t __user *count_props_ptr = (uint32_t __user *)(unsigned long)(arg->count_props_ptr);
	uint32_t __user *props_ptr = (uint32_t __user *)(unsigned long)(arg->props_ptr);
	uint64_t __user *prop_values_ptr = (uint64_t __user *)(unsigned long)(arg->prop_values_ptr);
	unsigned int copied_objs, copied_props;
	struct drm_atomic_state *state;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_out_fence_state *fence_state;
	int ret = 0;
	unsigned int i, j, num_fences;
	int a52u_req = 0;

	/* A52_PHASE446S_ATOMIC_IOCTL_TRACE_V1
	 * Caller/flags before commit; correlate using pid+timestamp with FRAME.
	 * Do not modify userspace state or ioctl return value.
	 */
	if (ktime_get_boottime_seconds() >= 85) {
		static atomic_t captured = ATOMIC_INIT(0);
		int seq = atomic_inc_return(&captured);
		if (seq <= 350)
			pr_info("P446S IOCTL seq=%d t_ns=%llu pid=%d comm=%s flags=%#x count_objs=%u TEST_ONLY=%u ALLOW_MODESET=%u NONBLOCK=%u\n",
				seq,(unsigned long long)ktime_get_boottime_ns(),
				task_pid_nr(current),current->comm,
				arg->flags,arg->count_objs,
				!!(arg->flags & DRM_MODE_ATOMIC_TEST_ONLY),
				!!(arg->flags & DRM_MODE_ATOMIC_ALLOW_MODESET),
				!!(arg->flags & DRM_MODE_ATOMIC_NONBLOCK));
	}
	/* disallow for drivers not supporting atomic: */
	if (!drm_core_check_feature(dev, DRIVER_ATOMIC))
		return -EOPNOTSUPP;

	/* disallow for userspace that has not enabled atomic cap (even
	 * though this may be a bit overkill, since legacy userspace
	 * wouldn't know how to call this ioctl)
	 */
	if (!file_priv->atomic)
		return -EINVAL;

	if (arg->flags & ~DRM_MODE_ATOMIC_FLAGS)
		return -EINVAL;

	if (arg->reserved)
		return -EINVAL;

	if (arg->flags & DRM_MODE_PAGE_FLIP_ASYNC)
		return -EINVAL;

	/* can't test and expect an event at the same time. */
	if ((arg->flags & DRM_MODE_ATOMIC_TEST_ONLY) &&
			(arg->flags & DRM_MODE_PAGE_FLIP_EVENT))
		return -EINVAL;

	state = drm_atomic_state_alloc(dev);
	if (!state)
		return -ENOMEM;

	drm_modeset_acquire_init(&ctx, DRM_MODESET_ACQUIRE_INTERRUPTIBLE);
	state->acquire_ctx = &ctx;
	state->allow_modeset = !!(arg->flags & DRM_MODE_ATOMIC_ALLOW_MODESET);

if (ktime_get_boottime_ns() >= 85ULL * 1000000000ULL) {
		a52u_req = atomic_inc_return(&a52u_ioctl_seq);
		if (a52u_req > 300) {
			int post = a52v_post_window_next();
			a52u_req = post ? 300 + post : 0;
		}
		if (a52u_req > 0 && a52u_req <= 400) {
			if (a52u_req == 301)
				pr_info("P446V POST_WINDOW_BEGIN ts=%llu count=100 trigger=NORMAL_DISPLAY_ON\n",
					(unsigned long long)ktime_get_boottime_ns());
			a52u_req_set(a52u_req);
			pr_info("P446U REQUEST seq=%d pid=%d comm=%s ts=%llu objs=%u flags=%#x\n",
				a52u_req, task_pid_nr(current), current->comm,
				(unsigned long long)ktime_get_boottime_ns(),
				arg->count_objs, arg->flags);
		}
	}

retry:
	if (!a52aa_seq) {
		a52aa_seq=atomic_inc_return(&a52aa_req_no);
			a52ab_count_hwc_ioctl(arg);
		a52ac_ring_start(a52aa_seq,arg);
			a52f_seq=a52f_begin(arg);
		if (a52aa_seq<=192)
			pr_info("P446AA DRM_IOCTL seq=%u pid=%d comm=%s flags=%#x objects=%u\n",
				a52aa_seq,task_pid_nr(current),current->comm,arg->flags,arg->count_objs);
	}
	a52f_retry(a52f_seq);
	copied_objs = 0;
	copied_props = 0;
	fence_state = NULL;
	num_fences = 0;

	for (i = 0; i < arg->count_objs; i++) {
		uint32_t obj_id, count_props;
		struct drm_mode_object *obj;

		if (get_user(obj_id, objs_ptr + copied_objs)) {
			ret = -EFAULT;
			goto out;
		}

		obj = drm_mode_object_find(dev, file_priv, obj_id, DRM_MODE_OBJECT_ANY);
		if (!obj) {
			ret = -ENOENT;
			goto out;
		}

		if (!obj->properties) {
			drm_mode_object_put(obj);
			ret = -ENOENT;
			goto out;
		}

		if (get_user(count_props, count_props_ptr + copied_objs)) {
			drm_mode_object_put(obj);
			ret = -EFAULT;
			goto out;
		}

		a52f_object(a52f_seq,obj,count_props);
		copied_objs++;

		for (j = 0; j < count_props; j++) {
			uint32_t prop_id;
			uint64_t prop_value;
			struct drm_property *prop;

			if (get_user(prop_id, props_ptr + copied_props)) {
				drm_mode_object_put(obj);
				ret = -EFAULT;
				goto out;
			}

			prop = drm_mode_obj_find_prop_id(obj, prop_id);
			if (!prop) {
				drm_mode_object_put(obj);
				ret = -ENOENT;
				goto out;
			}

			if (copy_from_user(&prop_value,
					   prop_values_ptr + copied_props,
					   sizeof(prop_value))) {
				drm_mode_object_put(obj);
				ret = -EFAULT;
				goto out;
			}

			if (a52u_req && a52u_req > 0 && a52u_req <= 400)
				if (a52_447c_trace_verbose()) pr_info("P446U PROP seq=%d pid=%d obj=%u type=%#x prop=%s val=%llu hex=%#llx\n",
					a52u_req, task_pid_nr(current), obj_id, obj->type,
					prop->name, (unsigned long long)prop_value,
					(unsigned long long)prop_value);

			if (a52aa_seq && a52aa_seq<=192 &&
				obj->type==DRM_MODE_OBJECT_PLANE &&
				(!strcmp(prop->name,"FB_ID") ||
				 !strcmp(prop->name,"CRTC_ID")))
				pr_info("P446AA DRM_INPUT seq=%u plane=%u prop=%s val=%llu\n",
					a52aa_seq,obj->id,prop->name,
					(unsigned long long)prop_value);
			a52ac_ring_prop(a52aa_seq,obj,prop,prop_value);
			a52f_property(a52f_seq,obj,prop,prop_value);
			ret = drm_atomic_set_property(state, file_priv,
						      obj, prop, prop_value);
			if (ret) {
				drm_mode_object_put(obj);
				goto out;
			}

			copied_props++;
		}

		drm_mode_object_put(obj);
	}

	ret = prepare_signaling(dev, state, arg, file_priv, &fence_state,
				&num_fences);
	if (ret)
		goto out;

	if (arg->flags & DRM_MODE_ATOMIC_TEST_ONLY) {
		ret = drm_atomic_check_only(state);
	} else if (arg->flags & DRM_MODE_ATOMIC_NONBLOCK) {
		ret = drm_atomic_nonblocking_commit(state);
	} else {
		if (drm_debug_enabled(DRM_UT_STATE))
			drm_atomic_print_state(state);

		ret = drm_atomic_commit(state);
	}

out:
	a52ac_ring_end(a52aa_seq);
	a52f_end(a52f_seq,ret);
	complete_signaling(dev, state, fence_state, num_fences, !ret);

	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry;
	}

	if (a52u_req && a52u_req > 0 && a52u_req <= 400)
		a52u_req_set(0);

	drm_atomic_state_put(state);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;
}

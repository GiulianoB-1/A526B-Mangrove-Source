/* A52_PHASE446W_HANDOFF_B_ACTUAL_FRAME_V2 */
#include <linux/a52_phase447.h> /* A52_PHASE447C_QUIET_PER_COMMIT_V1 */
#include <linux/a52_446w_handoff_b.h>
#include <linux/a52_446v_display_trace.h>
#include <linux/a52_446v_display_trace.h>
#include <linux/a52_446u_trace.h>
/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 * Copyright (C) 2014 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <drm/drm_panel.h>

#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_kms.h"
#include "sde_trace.h"
#include <linux/atomic.h>
#include <linux/a52_ack_secure_flight_recorder.h>

#if defined(CONFIG_DISPLAY_SAMSUNG)
#include "ss_dsi_panel_common.h"
#endif

#define MULTIPLE_CONN_DETECTED(x) (x > 1)

#define A52_R210_TRACE_LIMIT 8

static atomic_t a52_r210_commit_sequence = ATOMIC_INIT(0);

#define A52_R210_REC_ID(enabled, id, fmt, ...) \
	do { \
		if (enabled) \
			a52_ackfr_record("DRMPOST 210 c=%u " fmt, \
					  (unsigned int)(id), ##__VA_ARGS__); \
	} while (0)

#define A52_R210_REC(commit, fmt, ...) \
	A52_R210_REC_ID((commit)->a52_trace, (commit)->a52_trace_id, \
			  fmt, ##__VA_ARGS__)

struct msm_commit {
	struct drm_device *dev;
	struct drm_atomic_state *state;
	uint32_t crtc_mask;
	uint32_t plane_mask;
	bool nonblock;
	bool a52_trace;
	u32 a52_trace_id;
	struct kthread_work commit_work;
};

static inline bool _msm_seamless_for_crtc(struct drm_device *dev,
					struct drm_atomic_state *state,
			struct drm_crtc_state *crtc_state, bool enable)
{
	struct drm_connector *connector = NULL;
	struct drm_connector_state  *conn_state = NULL;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	int i = 0;
	int conn_cnt = 0;
	bool splash_en = false;

	if (msm_is_mode_seamless(&crtc_state->mode) ||
		msm_is_mode_seamless_vrr(&crtc_state->adjusted_mode) ||
		msm_is_mode_seamless_poms(&crtc_state->adjusted_mode) ||
		msm_is_mode_seamless_dyn_clk(&crtc_state->adjusted_mode))
		return true;

	if (msm_is_mode_seamless_dms(&crtc_state->adjusted_mode) && !enable)
		return true;

	if (!crtc_state->mode_changed && crtc_state->connectors_changed) {
		for_each_old_connector_in_state(state, connector,
				conn_state, i) {
			if ((conn_state->crtc == crtc_state->crtc) ||
					(connector->state->crtc ==
					 crtc_state->crtc))
				conn_cnt++;

			if (kms && kms->funcs && kms->funcs->check_for_splash)
				splash_en = kms->funcs->check_for_splash(kms,
							 crtc_state->crtc);

			if (MULTIPLE_CONN_DETECTED(conn_cnt) && !splash_en)
				return true;
		}
	}

	return false;
}

static inline bool _msm_seamless_for_conn(struct drm_connector *connector,
		struct drm_connector_state *old_conn_state, bool enable)
{
	if (!old_conn_state || !old_conn_state->crtc)
		return false;

	if (!old_conn_state->crtc->state->mode_changed &&
			!old_conn_state->crtc->state->active_changed &&
			old_conn_state->crtc->state->connectors_changed) {
		if (old_conn_state->crtc == connector->state->crtc)
			return true;
	}

	if (enable)
		return false;

	if (!connector->state->crtc &&
		old_conn_state->crtc->state->connectors_changed)
		return false;

	if (msm_is_mode_seamless(&connector->encoder->crtc->state->mode))
		return true;

	if (msm_is_mode_seamless_vrr(
			&connector->encoder->crtc->state->adjusted_mode))
		return true;

	if (msm_is_mode_seamless_dyn_clk(
			 &connector->encoder->crtc->state->adjusted_mode))
		return true;

	if (msm_is_mode_seamless_dms(
			&connector->encoder->crtc->state->adjusted_mode))
		return true;

	return false;
}

/* clear specified crtcs (no longer pending update) */
static void commit_destroy(struct msm_commit *c)
{
	struct msm_drm_private *priv = c->dev->dev_private;
	uint32_t crtc_mask = c->crtc_mask;
	uint32_t plane_mask = c->plane_mask;

	/* End_atomic */
	spin_lock(&priv->pending_crtcs_event.lock);
	DBG("end: %08x", crtc_mask);
	priv->pending_crtcs &= ~crtc_mask;
	priv->pending_planes &= ~plane_mask;
	wake_up_all_locked(&priv->pending_crtcs_event);
	spin_unlock(&priv->pending_crtcs_event.lock);

	if (c->nonblock)
		kfree(c);
}

static void msm_atomic_wait_for_commit_done(
		struct drm_device *dev,
		struct drm_atomic_state *old_state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *new_crtc_state;
	struct msm_drm_private *priv = old_state->dev->dev_private;
	struct msm_kms *kms = priv->kms;
	int i;

	for_each_new_crtc_in_state(old_state, crtc, new_crtc_state, i) {
		if (!new_crtc_state->active)
			continue;

		kms->funcs->wait_for_crtc_commit_done(kms, crtc);
	}
}

static void
msm_disable_outputs(struct drm_device *dev, struct drm_atomic_state *old_state)
{
	struct drm_connector *connector;
	struct drm_connector_state *old_conn_state;
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state;
	int i;

	SDE_ATRACE_BEGIN("msm_disable");
	for_each_old_connector_in_state(old_state, connector,
			old_conn_state, i) {
		const struct drm_encoder_helper_funcs *funcs;
		struct drm_encoder *encoder;
		struct drm_crtc_state *old_crtc_state;

		/*
		 * Shut down everything that's in the changeset and currently
		 * still on. So need to check the old, saved state.
		 */
		if (!old_conn_state->crtc)
			continue;

		old_crtc_state = drm_atomic_get_old_crtc_state(old_state,
							old_conn_state->crtc);

		if (!old_crtc_state->active ||
		    !drm_atomic_crtc_needs_modeset(old_conn_state->crtc->state))
			continue;

		encoder = old_conn_state->best_encoder;

		/* We shouldn't get this far if we didn't previously have
		 * an encoder.. but WARN_ON() rather than explode.
		 */
		if (WARN_ON(!encoder))
			continue;

		if (_msm_seamless_for_conn(connector, old_conn_state, false))
			continue;

		funcs = encoder->helper_private;

		DRM_DEBUG_ATOMIC("disabling [ENCODER:%d:%s]\n",
				 encoder->base.id, encoder->name);

		/*
		 * Each encoder has at most one connector (since we always steal
		 * it away), so we won't call disable hooks twice.
		 */
		drm_bridge_disable(encoder->bridge);

		/* Right function depends upon target state. */
		if (connector->state->crtc && funcs->prepare)
			funcs->prepare(encoder);
		else if (funcs->disable)
			funcs->disable(encoder);
		else
			funcs->dpms(encoder, DRM_MODE_DPMS_OFF);

#if defined(CONFIG_DISPLAY_SAMSUNG)
		{
			/*
				notify registered clients about suspend event
				This noti is triggered before panel power off & DSI_CMD_SET_OFF.
			*/
			int blank = FB_BLANK_POWERDOWN;

			if (encoder->encoder_type == DRM_MODE_ENCODER_DSI &&
				(connector->state->crtc &&
					connector->state->crtc->state->active_changed))
				__msm_drm_notifier_call_chain(FB_EVENT_BLANK, &blank);
			else
				pr_debug("%s %d\n", __func__, encoder->encoder_type);
		}
#endif

		drm_bridge_post_disable(encoder->bridge);
	}

	for_each_old_crtc_in_state(old_state, crtc, old_crtc_state, i) {
		const struct drm_crtc_helper_funcs *funcs;

		/* Shut down everything that needs a full modeset. */
		if (!drm_atomic_crtc_needs_modeset(crtc->state))
			continue;

		if (!old_crtc_state->active)
			continue;

		if (_msm_seamless_for_crtc(dev, old_state, crtc->state, false))
			continue;

		funcs = crtc->helper_private;

		DRM_DEBUG_ATOMIC("disabling [CRTC:%d]\n",
				 crtc->base.id);

		/* Right function depends upon target state. */
		if (crtc->state->enable && funcs->prepare)
			funcs->prepare(crtc);
		else if (funcs->disable)
			funcs->disable(crtc);
		else
			funcs->dpms(crtc, DRM_MODE_DPMS_OFF);
	}
	SDE_ATRACE_END("msm_disable");
}

static void
msm_crtc_set_mode(struct drm_device *dev, struct drm_atomic_state *old_state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state;
	struct drm_connector *connector;
	struct drm_connector_state *old_conn_state;
	int i;

	for_each_old_crtc_in_state(old_state, crtc, old_crtc_state, i) {
		const struct drm_crtc_helper_funcs *funcs;

		if (!crtc->state->mode_changed)
			continue;

		funcs = crtc->helper_private;

		if (crtc->state->enable && funcs->mode_set_nofb) {
			DRM_DEBUG_ATOMIC("modeset on [CRTC:%d]\n",
					 crtc->base.id);

			funcs->mode_set_nofb(crtc);
		}
	}

	for_each_old_connector_in_state(old_state, connector,
			old_conn_state, i) {
		const struct drm_encoder_helper_funcs *funcs;
		struct drm_crtc_state *new_crtc_state;
		struct drm_encoder *encoder;
		struct drm_display_mode *mode, *adjusted_mode;

		if (!connector->state->best_encoder)
			continue;

		encoder = connector->state->best_encoder;
		funcs = encoder->helper_private;
		new_crtc_state = connector->state->crtc->state;
		mode = &new_crtc_state->mode;
		adjusted_mode = &new_crtc_state->adjusted_mode;

		if (!new_crtc_state->mode_changed &&
				new_crtc_state->connectors_changed) {
			if (_msm_seamless_for_conn(connector,
					old_conn_state, false))
				continue;
		} else if (!new_crtc_state->mode_changed) {
			continue;
		}

		DRM_DEBUG_ATOMIC("modeset on [ENCODER:%d:%s]\n",
				 encoder->base.id, encoder->name);

		SDE_ATRACE_BEGIN("msm_set_mode");
		/*
		 * Each encoder has at most one connector (since we always steal
		 * it away), so we won't call mode_set hooks twice.
		 */
		if (funcs->mode_set)
			funcs->mode_set(encoder, mode, adjusted_mode);

		drm_bridge_mode_set(encoder->bridge, mode, adjusted_mode);
		SDE_ATRACE_END("msm_set_mode");
	}
}

/**
 * msm_atomic_helper_commit_modeset_disables - modeset commit to disable outputs
 * @dev: DRM device
 * @old_state: atomic state object with old state structures
 *
 * This function shuts down all the outputs that need to be shut down and
 * prepares them (if required) with the new mode.
 *
 * For compatibility with legacy crtc helpers this should be called before
 * drm_atomic_helper_commit_planes(), which is what the default commit function
 * does. But drivers with different needs can group the modeset commits together
 * and do the plane commits at the end. This is useful for drivers doing runtime
 * PM since planes updates then only happen when the CRTC is actually enabled.
 */
void msm_atomic_helper_commit_modeset_disables(struct drm_device *dev,
		struct drm_atomic_state *old_state)
{
	msm_disable_outputs(dev, old_state);

	drm_atomic_helper_update_legacy_modeset_state(dev, old_state);

	msm_crtc_set_mode(dev, old_state);
}

/**
 * msm_atomic_helper_commit_modeset_enables - modeset commit to enable outputs
 * @dev: DRM device
 * @old_state: atomic state object with old state structures
 *
 * This function enables all the outputs with the new configuration which had to
 * be turned off for the update.
 *
 * For compatibility with legacy crtc helpers this should be called after
 * drm_atomic_helper_commit_planes(), which is what the default commit function
 * does. But drivers with different needs can group the modeset commits together
 * and do the plane commits at the end. This is useful for drivers doing runtime
 * PM since planes updates then only happen when the CRTC is actually enabled.
 */
extern void a52_p446_mark(u32 event,u32 aux0,u32 aux1);
/* A52_PHASE446B_H12_BRIDGE_PRE_ENABLE */
static void msm_atomic_helper_commit_modeset_enables(struct drm_device *dev,
		struct drm_atomic_state *old_state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state;
	struct drm_crtc_state *new_crtc_state;
	struct drm_connector *connector;
	struct drm_connector_state *new_conn_state;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	int bridge_enable_count = 0;
	int i;

	SDE_ATRACE_BEGIN("msm_enable");
	for_each_oldnew_crtc_in_state(old_state, crtc, old_crtc_state,
			new_crtc_state, i) {
		const struct drm_crtc_helper_funcs *funcs;

		/* Need to filter out CRTCs where only planes change. */
		if (!drm_atomic_crtc_needs_modeset(new_crtc_state))
			continue;

		if (!new_crtc_state->active)
			continue;

		if (_msm_seamless_for_crtc(dev, old_state, crtc->state, true))
			continue;

		funcs = crtc->helper_private;

		if (crtc->state->enable) {
			DRM_DEBUG_ATOMIC("enabling [CRTC:%d]\n",
					 crtc->base.id);

			if (funcs->atomic_enable)
				funcs->atomic_enable(crtc, old_crtc_state);
			else
				funcs->commit(crtc);
		}

		if (msm_needs_vblank_pre_modeset(
					&new_crtc_state->adjusted_mode))
			drm_crtc_wait_one_vblank(crtc);

	}

	for_each_new_connector_in_state(old_state, connector,
			new_conn_state, i) {
		const struct drm_encoder_helper_funcs *funcs;
		struct drm_encoder *encoder;
		struct drm_connector_state *old_conn_state;

		if (!new_conn_state->best_encoder)
			continue;

		if (!new_conn_state->crtc->state->active ||
				!drm_atomic_crtc_needs_modeset(
					new_conn_state->crtc->state))
			continue;

		old_conn_state = drm_atomic_get_old_connector_state(
				old_state, connector);
		if (_msm_seamless_for_conn(connector, old_conn_state, true))
			continue;

		encoder = connector->state->best_encoder;
		funcs = encoder->helper_private;

		DRM_DEBUG_ATOMIC("enabling [ENCODER:%d:%s]\n",
				 encoder->base.id, encoder->name);

		/*
		 * Each encoder has at most one connector (since we always steal
		 * it away), so we won't call enable hooks twice.
		 */
		a52_p446_mark(0x12cU,(u32)encoder->base.id,(u32)connector->base.id);
		drm_bridge_pre_enable(encoder->bridge);
		++bridge_enable_count;

		if (funcs->enable)
			funcs->enable(encoder);
		else
			funcs->commit(encoder);

#if defined(CONFIG_DISPLAY_SAMSUNG)
		{
			/*
				notify registered clients about resume event.
				This noti is triggered after panel power on & DSI_CMD_SET_ON.
			*/
			int blank = FB_BLANK_UNBLANK;

			/* notify only in case state is changed (off -> on) */
			if (encoder->encoder_type == DRM_MODE_ENCODER_DSI &&
				(connector->state->crtc &&
					connector->state->crtc->state->active_changed))
				__msm_drm_notifier_call_chain(FB_EVENT_BLANK, &blank);
			else
				pr_debug("%s %d\n", __func__, encoder->encoder_type);
		}
#endif

	}

	if (kms && kms->funcs && kms->funcs->commit) {
		DRM_DEBUG_ATOMIC("triggering commit\n");
		kms->funcs->commit(kms, old_state);
	}

	/* If no bridges were pre_enabled, skip iterating over them again */
	if (bridge_enable_count == 0) {
		SDE_ATRACE_END("msm_enable");
		return;
	}

	for_each_new_connector_in_state(old_state, connector,
			new_conn_state, i) {
		struct drm_encoder *encoder;
		struct drm_connector_state *old_conn_state;

		if (!new_conn_state->best_encoder)
			continue;

		if (!new_conn_state->crtc->state->active ||
		    !drm_atomic_crtc_needs_modeset(
				    new_conn_state->crtc->state))
			continue;

		old_conn_state = drm_atomic_get_old_connector_state(
				old_state, connector);
		if (_msm_seamless_for_conn(connector, old_conn_state, true))
			continue;

		encoder = connector->state->best_encoder;

		DRM_DEBUG_ATOMIC("bridge enable enabling [ENCODER:%d:%s]\n",
				 encoder->base.id, encoder->name);

		drm_bridge_enable(encoder->bridge);
	}
	SDE_ATRACE_END("msm_enable");
}

int msm_atomic_prepare_fb(struct drm_plane *plane,
			  struct drm_plane_state *new_state)
{
	struct msm_drm_private *priv = plane->dev->dev_private;
	struct msm_kms *kms = priv->kms;
	struct drm_gem_object *obj;
	struct msm_gem_object *msm_obj;
	struct dma_fence *fence;

	if (!new_state->fb)
		return 0;

	obj = msm_framebuffer_bo(new_state->fb, 0);
	msm_obj = to_msm_bo(obj);
	fence = dma_resv_get_excl_rcu(msm_obj->resv);

	drm_atomic_set_fence_for_plane(new_state, fence);

	return msm_framebuffer_prepare(new_state->fb, kms->aspace);
}

#if defined(CONFIG_DISPLAY_SAMSUNG)
int ss_get_vdd_ndx_from_state(struct drm_atomic_state *old_state);
#endif

/* The (potentially) asynchronous part of the commit.  At this point
 * nothing can fail short of armageddon.
 */
static void complete_commit(struct msm_commit *c)
{
	struct drm_atomic_state *state = c->state;
	struct drm_device *dev = state->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
#if defined(CONFIG_DISPLAY_SAMSUNG)
	struct samsung_display_driver_data *vdd;
	int ndx;
#endif

	a52_ackfr_record("P276 296W e");
	A52_R210_REC(c, "complete enter");
	A52_R210_REC(c, "fences wait enter");
	drm_atomic_helper_wait_for_fences(dev, state, false);
	A52_R210_REC(c, "fences wait exit");

	A52_R210_REC(c, "prepare_commit enter");
	kms->funcs->prepare_commit(kms, state);
	A52_R210_REC(c, "prepare_commit exit");

	A52_R210_REC(c, "modeset_disable enter");
	if (a52w_handoff_selected() && !a52w_handoff_done())
		pr_info("P446W HANDOFF phase=disable-enter\n");
	msm_atomic_helper_commit_modeset_disables(dev, state);
	if (a52w_handoff_selected() && !a52w_handoff_done())
		pr_info("P446W HANDOFF phase=disable-exit\n");
	A52_R210_REC(c, "modeset_disable exit");

	A52_R210_REC(c, "planes enter");
	drm_atomic_helper_commit_planes(dev, state,
				DRM_PLANE_COMMIT_ACTIVE_ONLY);
	A52_R210_REC(c, "planes exit");

	A52_R210_REC(c, "modeset_enable enter");
	if (a52w_handoff_selected() && !a52w_handoff_done())
		pr_info("P446W HANDOFF phase=enable-enter\n");
	msm_atomic_helper_commit_modeset_enables(dev, state);
	if (a52w_handoff_selected() && !a52w_handoff_done())
		pr_info("P446W HANDOFF phase=enable-exit\n");
	A52_R210_REC(c, "modeset_enable exit");

	/* NOTE: _wait_for_vblanks() only waits for vblank on
	 * enabled CRTCs.  So we end up faulting when disabling
	 * due to (potentially) unref'ing the outgoing fb's
	 * before the vblank when the disable has latched.
	 *
	 * But if it did wait on disabled (or newly disabled)
	 * CRTCs, that would be racy (ie. we could have missed
	 * the irq.  We need some way to poll for pipe shut
	 * down.  Or just live with occasionally hitting the
	 * timeout in the CRTC disable path (which really should
	 * not be critical path)
	 */

	A52_R210_REC(c, "wait_done enter");
	msm_atomic_wait_for_commit_done(dev, state);
	A52_R210_REC(c, "wait_done exit");

#if defined(CONFIG_DISPLAY_SAMSUNG)
	ndx = ss_get_vdd_ndx_from_state(state);
	vdd = ss_get_vdd(ndx);
	if (vdd)
		ss_event_frame_update_post(vdd);
#endif

	A52_R210_REC(c, "cleanup enter");
	drm_atomic_helper_cleanup_planes(dev, state);
	A52_R210_REC(c, "cleanup exit");

	A52_R210_REC(c, "complete_cb enter");
	if (a52w_handoff_selected() && !a52w_handoff_done())
		pr_info("P446W HANDOFF phase=sde_complete-enter\n");
	kms->funcs->complete_commit(kms, state);
	if (a52w_handoff_selected() && !a52w_handoff_done())
		pr_info("P446W HANDOFF phase=sde_complete-exit\n");
	A52_R210_REC(c, "complete_cb exit");

	/* SDE complete_commit has now released splash resources, if eligible.
	 * Only a committed full DRM modeset may close the once-only gate.
	 */
	if (a52w_handoff_selected() && !a52w_handoff_done()) {
		struct drm_crtc *a52w_crtc;
		struct drm_crtc_state *a52w_cs;
		int a52w_i;
		for_each_new_crtc_in_state(state,a52w_crtc,a52w_cs,a52w_i) {
			if (a52w_crtc->base.id==107 &&
			    a52w_cs->active && a52w_cs->mode_changed &&
			    a52w_cs->connectors_changed) {
				a52w_handoff_finish(dev);
				break;
			}
		}
	}
	drm_atomic_state_put(state);

	A52_R210_REC(c, "complete exit");
	commit_destroy(c);
}

static void _msm_drm_commit_work_cb(struct kthread_work *work)
{
	struct msm_commit *commit = NULL;

	if (!work) {
		DRM_ERROR("%s: Invalid commit work data!\n", __func__);
		return;
	}

	commit = container_of(work, struct msm_commit, commit_work);

	SDE_ATRACE_BEGIN("complete_commit");
	complete_commit(commit);
	SDE_ATRACE_END("complete_commit");
}

static struct msm_commit *commit_init(struct drm_atomic_state *state,
	bool nonblock)
{
	struct msm_commit *c = kzalloc(sizeof(*c), GFP_KERNEL);

	if (!c)
		return NULL;

	c->dev = state->dev;
	c->state = state;
	c->nonblock = nonblock;

	kthread_init_work(&c->commit_work, _msm_drm_commit_work_cb);

	return c;
}

/* Start display thread function */
static void msm_atomic_commit_dispatch(struct drm_device *dev,
		struct drm_atomic_state *state, struct msm_commit *commit)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_crtc *crtc = NULL;
	struct drm_crtc_state *crtc_state = NULL;
	int ret = -ECANCELED, i = 0, j = 0;
	bool nonblock;
	bool a52_trace;
	u32 a52_trace_id;

	/* cache since work may free commit in the non-blocking case */
	nonblock = commit->nonblock;
	a52_trace = commit->a52_trace;
	a52_trace_id = commit->a52_trace_id;
	A52_R210_REC_ID(a52_trace, a52_trace_id, "dispatch enter nb=%d",
			  nonblock);

	for_each_old_crtc_in_state(state, crtc, crtc_state, i) {
		for (j = 0; j < priv->num_crtcs; j++) {
			if (priv->disp_thread[j].crtc_id ==
						crtc->base.id) {
				if (priv->disp_thread[j].thread) {
					kthread_queue_work(
						&priv->disp_thread[j].worker,
							&commit->commit_work);
					/* only return zero if work is
					 * queued successfully.
					 */
					ret = 0;
					A52_R210_REC_ID(a52_trace, a52_trace_id,
							  "dispatch queued crtc=%u",
							  crtc->base.id);
				} else {
					DRM_ERROR(" Error for crtc_id: %d\n",
						priv->disp_thread[j].crtc_id);
					ret = -EINVAL;
				}
				break;
			}
		}
		/*
		 * TODO: handle cases where there will be more than
		 * one crtc per commit cycle. Remove this check then.
		 * Current assumption is there will be only one crtc
		 * per commit cycle.
		 */
		if (j < priv->num_crtcs)
			break;
	}

	A52_R210_REC_ID(a52_trace, a52_trace_id, "dispatch result rc=%d", ret);
	if (ret) {
		if (ret == -EINVAL)
			DRM_ERROR("failed to dispatch commit to any CRTC\n");
		else
			DRM_DEBUG_DRIVER_RATELIMITED("empty crtc state\n");

		/**
		 * this is not expected to happen, but at this point the state
		 * has been swapped, but we couldn't dispatch to a crtc thread.
		 * fallback now to a synchronous complete_commit to try and
		 * ensure that SW and HW state don't get out of sync.
		 */
		A52_R210_REC_ID(a52_trace, a52_trace_id, "dispatch fallback");
		complete_commit(commit);
	} else if (!nonblock) {
		A52_R210_REC_ID(a52_trace, a52_trace_id, "dispatch flush enter");
		kthread_flush_work(&commit->commit_work);
		A52_R210_REC_ID(a52_trace, a52_trace_id, "dispatch flush exit");
	}

	/* free nonblocking commits in this context, after processing */
	if (!nonblock)
		kfree(commit);
}

/**
 * drm_atomic_helper_commit - commit validated state object
 * @dev: DRM device
 * @state: the driver state object
 * @nonblock: nonblocking commit
 *
 * This function commits a with drm_atomic_helper_check() pre-validated state
 * object. This can still fail when e.g. the framebuffer reservation fails.
 *
 * RETURNS
 * Zero for success or -errno.
 */

#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/string.h>

/* A52_PHASE446S_UNSAMPLED_COMMIT_V1 */
/* A52_PHASE446T_EARLY_UNSAMPLED_FRAME_V1 */
static atomic_t p446t_frame_early_seq = ATOMIC_INIT(0);
static atomic_t p446s_frame_seq = ATOMIC_INIT(0);
static atomic_t p446s_bucket_count = ATOMIC_INIT(0);
static atomic_t p446s_bucket_zeros = ATOMIC_INIT(0);
static atomic_t p446s_bucket = ATOMIC_INIT(-1);

/* A52_PHASE446R_COMMIT_GEM_LIFETIME_V1
 * One line per selected submitted DRM commit. Plane states below are the
 * current *post-swap* composition, not only planes changed in this commit.
 * Black pixel content cannot be inferred from these metadata fields.
 */
static atomic_t p446r_commit_seq = ATOMIC_INIT(0);
static void p446r_commit_snapshot(struct drm_device *dev,
                int result, bool nonblock, const char *stage)
{
    struct drm_plane *plane;
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb;
    u32 fb_id[4]={0}, format[4]={0}, plane_id[4]={0};
    int n=atomic_inc_return(&p446r_commit_seq);
    int total=0, active=0, ncrtcs=0, n_enabled=0;
    u64 now_ns=ktime_get_boottime_ns();
    int win=-1, bucket;

    bool print;
    drm_for_each_crtc(crtc,dev) {
        if (crtc->state && crtc->state->active)
            ncrtcs++;
        if (crtc->state && crtc->state->enable)
            n_enabled++;
    }
    drm_for_each_plane(plane,dev) {
        total++;
        if (!plane->state || !plane->state->crtc ||
            !plane->state->fb)
            continue;
        fb=plane->state->fb;
        if (active<4) {
            plane_id[active]=plane->base.id;
            fb_id[active]=fb->base.id;
            format[active]=fb->format ? fb->format->format : 0;
        }
        active++;
    }
    /* A52_PHASE446T_EARLY_UNSAMPLED_FRAME_V1: one line per swapped
     * commit after 15s, no modulo selection, while preserving the original
     * 446S >85s 300-commit window. Metadata only, no framebuffer reads.
     */
    if (now_ns >= 15ULL * 1000000000ULL &&
        now_ns < 85ULL * 1000000000ULL &&
        !strcmp(stage, "swapped")) {
        int seq = atomic_inc_return(&p446t_frame_early_seq);

        if (seq <= 300)
            if (a52_447c_trace_verbose()) pr_info("P446T FRAME_EARLY seq=%d t_ns=%llu pid=%d comm=%s rc=%d nonblock=%u crtcs=%d enabled=%d planes=%d fb0=%u fmt0=%08x fb1=%u fmt1=%08x\n",
                seq, (unsigned long long)now_ns,
                task_pid_nr(current), current->comm, result, nonblock,
                ncrtcs, n_enabled, active, fb_id[0], format[0],
                fb_id[1], format[1]);
    }
    /* 85-SECOND START. Log consecutive 300 commits without modulo sampling. */
    if (now_ns >= 85ULL * 1000000000ULL)
        win=atomic_inc_return(&p446s_frame_seq);

    if (win > 0 && win <= 300)
        if (a52_447c_trace_verbose()) pr_info("P446U FRAME_JOIN seq=%d req=%d pid=%d\n",
            win, a52_446u_request_id(), task_pid_nr(current));
    if (win > 0 && win <= 300) {
        if (a52_447c_trace_verbose()) pr_info("P446S FRAME seq=%d n=%d t_ns=%llu pid=%d comm=%s rc=%d nonblock=%u crtc_active=%d crtc_enable=%d active_planes=%d p0=%u/fb%u/fmt%08x p1=%u/fb%u/fmt%08x p2=%u/fb%u/fmt%08x p3=%u/fb%u/fmt%08x\n",
            win,n,(unsigned long long)now_ns,task_pid_nr(current),current->comm,
            result,nonblock,ncrtcs,n_enabled,active,
            plane_id[0],fb_id[0],format[0],
            plane_id[1],fb_id[1],format[1],
            plane_id[2],fb_id[2],format[2],
            plane_id[3],fb_id[3],format[3]);
    }
    if (win > 0) {
        int prev;
        bucket = (int)(now_ns / 5000000000ULL);
        atomic_inc(&p446s_bucket_count);
        if (!active) atomic_inc(&p446s_bucket_zeros);
        prev=atomic_read(&p446s_bucket);
        if (prev < 0) {
            atomic_cmpxchg(&p446s_bucket,prev,bucket);
        } else if (prev != bucket &&
                   atomic_cmpxchg(&p446s_bucket,prev,bucket)==prev) {
            int all=atomic_xchg(&p446s_bucket_count,0);
            int empty=atomic_xchg(&p446s_bucket_zeros,0);
            pr_info("P446S FRAME_SUMMARY 5s_bucket=%d commits=%d zero_planes=%d zero_per_second_x100=%d\n",
                prev,all,empty,empty*20);
        }
    }
    print=(n <= 96) || (n%15 == 0) || result;
    if (print)
        pr_info("P446R COMMIT n=%d step=%s rc=%d nb=%u crtcs=%d active_planes=%d planes_total=%d p0=%u/fb%u/fmt%08x p1=%u/fb%u/fmt%08x p2=%u/fb%u/fmt%08x p3=%u/fb%u/fmt%08x\n",
            n,stage,result,nonblock,ncrtcs,active,total,
            plane_id[0],fb_id[0],format[0],
            plane_id[1],fb_id[1],format[1],
            plane_id[2],fb_id[2],format[2],
            plane_id[3],fb_id[3],format[3]);
}


/* A52_PHASE446U_ATOMIC_REQUEST_PLANE91_V1 */
static void a52u_snapshot_plane91(struct drm_device *dev, const char *when)
{
	struct drm_plane *plane;
	if (ktime_get_boottime_ns() < 85ULL * 1000000000ULL)
		return;
	drm_for_each_plane(plane, dev) {
		struct drm_plane_state *s;
		if (plane->base.id != 91)
			continue;
		s = plane->state;
		if (a52_447c_trace_verbose()) pr_info("P446U PLANE91 seq=%d phase=%s ts=%llu fb=%u crtc=%u visible=%u\n",
			a52_446u_request_id(), when,
			(unsigned long long)ktime_get_boottime_ns(),
			s && s->fb ? s->fb->base.id : 0,
			s && s->crtc ? s->crtc->base.id : 0,
			s ? s->visible : 0);
	}
}

/* A52_PHASE446Y_HANDOFF_SE8_V2 */
static atomic_t a52y_handoff_busy = ATOMIC_INIT(0);

int msm_atomic_commit(struct drm_device *dev,
		struct drm_atomic_state *state, bool nonblock)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_commit *c;
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_plane *plane;
	struct drm_plane_state *old_plane_state, *new_plane_state;
	int i, ret;
	bool a52y_owner = false;
	bool a52y_first_modeset = false;
	int a52_trace_index;
	bool a52_trace;

#if defined(CONFIG_DISPLAY_SAMSUNG)
	struct samsung_display_driver_data *vdd;
	int ndx;
#endif

	a52_ackfr_record("P276 296C e n=%d", nonblock);
	a52_trace_index = atomic_inc_return(&a52_r210_commit_sequence);
	a52_trace = a52_trace_index <= A52_R210_TRACE_LIMIT;
	A52_R210_REC_ID(a52_trace, a52_trace_index,
			  "commit enter nb=%d", nonblock);

	if (!priv || priv->shutdown_in_progress) {
		A52_R210_REC_ID(a52_trace, a52_trace_index,
				  "commit reject priv=%d shutdown=%d",
				  !priv, priv ? priv->shutdown_in_progress : -1);
		DRM_ERROR("priv is null or shutdwon is in-progress\n");
		return -EINVAL;
	}

/* Pending-CRTC reservation serializes against commits already in flight.
     * All NEW concurrent commits are rejected before Samsung pre-update;
     * the one forced transaction blocks until its commit worker finishes.
     */
    if (atomic_read(&a52y_handoff_busy)) {
        pr_info_ratelimited("P446Y HANDOFF concurrent reject rc=-EBUSY\n");
        return -EBUSY;
    }
    if (a52w_handoff_selected() && !a52w_handoff_done()) {
        for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
            if (crtc && crtc->base.id == 107 && crtc_state &&
                crtc_state->mode_changed && crtc_state->connectors_changed) {
                a52y_first_modeset = true;
                break;
            }
        }
        if (a52y_first_modeset) {
            if (atomic_cmpxchg(&a52y_handoff_busy, 0, 1)) {
                pr_info_ratelimited("P446Y HANDOFF duplicate reject rc=-EBUSY\n");
                return -EBUSY;
            }
            a52y_owner = true;
            nonblock = false;
            pr_info("P446Y HANDOFF BLOCKING begin pid=%d crtc=107\n",
                    task_pid_nr(current));
        }
    }

#if defined(CONFIG_DISPLAY_SAMSUNG)
	ndx = ss_get_vdd_ndx_from_state(state);
	vdd = ss_get_vdd(ndx);
	if (vdd)
		ss_event_frame_update_pre(vdd);
#endif

	SDE_ATRACE_BEGIN("atomic_commit");
	ret = drm_atomic_helper_prepare_planes(dev, state);
	A52_R210_REC_ID(a52_trace, a52_trace_index, "prepare_planes rc=%d", ret);
	if (ret) {
		a52_ackfr_record("P276 296C x r=%d q=1", ret);
		p446r_commit_snapshot(dev, ret, nonblock, "prepare");
		SDE_ATRACE_END("atomic_commit");
		if (a52y_owner)
			atomic_set(&a52y_handoff_busy, 0);
		return ret;
	}

	c = commit_init(state, nonblock);
	if (c) {
		c->a52_trace = a52_trace;
		c->a52_trace_id = (u32)a52_trace_index;
	}
	A52_R210_REC_ID(a52_trace, a52_trace_index, "commit_init ok=%d", !!c);
	if (!c) {
		ret = -ENOMEM;
		goto error;
	}

	/*
	 * Figure out what crtcs we have:
	 */
	for_each_new_crtc_in_state(state, crtc, crtc_state, i)
		c->crtc_mask |= drm_crtc_mask(crtc);

	/*
	 * Figure out what fence to wait for:
	 */
	for_each_oldnew_plane_in_state(state, plane, old_plane_state,
			new_plane_state, i) {
		if ((new_plane_state->fb != old_plane_state->fb)
				&& new_plane_state->fb) {
			struct drm_gem_object *obj =
				msm_framebuffer_bo(new_plane_state->fb, 0);
			struct msm_gem_object *msm_obj = to_msm_bo(obj);
			struct dma_fence *fence =
				dma_resv_get_excl_rcu(msm_obj->resv);

			drm_atomic_set_fence_for_plane(new_plane_state, fence);
		}
		c->plane_mask |= (1 << drm_plane_index(plane));
	}

	A52_R210_REC(c, "masks crtc=0x%x plane=0x%x",
			c->crtc_mask, c->plane_mask);

	/* Protection for prepare_fence callback */
retry:
	A52_R210_REC(c, "conn_lock enter");
	ret = drm_modeset_lock(&state->dev->mode_config.connection_mutex,
		state->acquire_ctx);
	A52_R210_REC(c, "conn_lock rc=%d", ret);

	if (ret == -EDEADLK) {
		drm_modeset_backoff(state->acquire_ctx);
		goto retry;
	}

	/*
	 * Wait for pending updates on any of the same crtc's and then
	 * mark our set of crtc's as busy:
	 */

	/* Start Atomic */
	A52_R210_REC(c, "pending wait enter pc=0x%x pp=0x%x",
			priv->pending_crtcs, priv->pending_planes);
	spin_lock(&priv->pending_crtcs_event.lock);
	ret = wait_event_interruptible_locked(priv->pending_crtcs_event,
			!(priv->pending_crtcs & c->crtc_mask) &&
			!(priv->pending_planes & c->plane_mask));
	if (ret == 0) {
		DBG("start: %08x", c->crtc_mask);
		priv->pending_crtcs |= c->crtc_mask;
		priv->pending_planes |= c->plane_mask;
	}
	spin_unlock(&priv->pending_crtcs_event.lock);
	A52_R210_REC(c, "pending wait exit rc=%d", ret);

	if (ret)
		goto err_free;

	A52_R210_REC(c, "swap_state enter");
	a52v_atomic_snapshot(dev);
	a52u_snapshot_plane91(dev, "before");
	WARN_ON(drm_atomic_helper_swap_state(state, false) < 0);
	A52_R210_REC(c, "swap_state exit");
	a52u_snapshot_plane91(dev, "after");
	p446r_commit_snapshot(dev, 0, nonblock, "swapped");

	/*
	 * Provide the driver a chance to prepare for output fences. This is
	 * done after the point of no return, but before asynchronous commits
	 * are dispatched to work queues, so that the fence preparation is
	 * finished before the .atomic_commit returns.
	 */
	if (priv && priv->kms && priv->kms->funcs &&
			priv->kms->funcs->prepare_fence) {
		A52_R210_REC(c, "prepare_fence enter");
		priv->kms->funcs->prepare_fence(priv->kms, state);
		A52_R210_REC(c, "prepare_fence exit");
	}

	/*
	 * Everything below can be run asynchronously without the need to grab
	 * any modeset locks at all under one conditions: It must be guaranteed
	 * that the asynchronous work has either been cancelled (if the driver
	 * supports it, which at least requires that the framebuffers get
	 * cleaned up with drm_atomic_helper_cleanup_planes()) or completed
	 * before the new state gets committed on the software side with
	 * drm_atomic_helper_swap_state().
	 *
	 * This scheme allows new atomic state updates to be prepared and
	 * checked in parallel to the asynchronous completion of the previous
	 * update. Which is important since compositors need to figure out the
	 * composition of the next frame right after having submitted the
	 * current layout
	 */

	drm_atomic_state_get(state);
	A52_R210_REC(c, "dispatch call");
	msm_atomic_commit_dispatch(dev, state, c);
	if (a52y_owner) {
		pr_info("P446Y HANDOFF BLOCKING end done=%d\n", a52w_handoff_done());
		atomic_set(&a52y_handoff_busy, 0);
	}

	SDE_ATRACE_END("atomic_commit");
	A52_R210_REC_ID(a52_trace, a52_trace_index, "commit return rc=0");
	a52_ackfr_record("P276 296C x r=0 q=0");

	return 0;
err_free:
	A52_R210_REC_ID(a52_trace, a52_trace_index, "commit err_free rc=%d", ret);
	kfree(c);
error:
	A52_R210_REC_ID(a52_trace, a52_trace_index, "commit error rc=%d", ret);
	p446r_commit_snapshot(dev, ret, nonblock, "error");
	drm_atomic_helper_cleanup_planes(dev, state);
	SDE_ATRACE_END("atomic_commit");
	a52_ackfr_record("P276 296C x r=%d q=2", ret);
	if (a52y_owner)
		atomic_set(&a52y_handoff_busy, 0);
	return ret;
}

struct drm_atomic_state *msm_atomic_state_alloc(struct drm_device *dev)
{
	struct msm_kms_state *state = kzalloc(sizeof(*state), GFP_KERNEL);

	if (!state || drm_atomic_state_init(dev, &state->base) < 0) {
		kfree(state);
		return NULL;
	}

	return &state->base;
}

void msm_atomic_state_clear(struct drm_atomic_state *s)
{
	struct msm_kms_state *state = to_kms_state(s);

	drm_atomic_state_default_clear(&state->base);
	kfree(state->state);
	state->state = NULL;
}

void msm_atomic_state_free(struct drm_atomic_state *state)
{
	kfree(to_kms_state(state)->state);
	drm_atomic_state_default_release(state);
	kfree(state);
}

void msm_atomic_commit_tail(struct drm_atomic_state *state)
{
	struct drm_device *dev = state->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;

	kms->funcs->prepare_commit(kms, state);

	drm_atomic_helper_commit_modeset_disables(dev, state);

	drm_atomic_helper_commit_planes(dev, state, 0);

	drm_atomic_helper_commit_modeset_enables(dev, state);

	msm_atomic_wait_for_commit_done(dev, state);

	kms->funcs->complete_commit(kms, state);

	drm_atomic_helper_wait_for_vblanks(dev, state);

	drm_atomic_helper_commit_hw_done(state);

	drm_atomic_helper_cleanup_planes(dev, state);
}

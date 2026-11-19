/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_A52_ACK_SECURE_FLIGHT_RECORDER_H
#define _LINUX_A52_ACK_SECURE_FLIGHT_RECORDER_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/types.h>

void a52_ackfr_record(const char *fmt, ...) __printf(1, 2);


/* A52_FAILURE_WINDOW_PROBE_V1 */
struct a52_ackfr_scope {
	const char *domain;
	const char *name;
	u64 start_ns;
};

struct a52_ackfr_scope a52_ackfr_scope_begin(const char *domain,
					      const char *name);
void a52_ackfr_scope_cleanup(struct a52_ackfr_scope *scope);

#define A52_ACKFR_SCOPE(domain, name) \
	struct a52_ackfr_scope __a52_ackfr_scope __maybe_unused \
	__attribute__((cleanup(a52_ackfr_scope_cleanup))) = \
		a52_ackfr_scope_begin((domain), (name))

/* A52_PHASE382_STICKY_USB_CLIFF_CENSUS_V1 */
enum a52_ackfr_usbdiag_field {
	A52_ACKFR_USB_MODE_IN = 0,
	A52_ACKFR_USB_HW_MODE,
	A52_ACKFR_USB_MODE_OUT,
	A52_ACKFR_USB_CORE_MODE,
	A52_ACKFR_USB_GADGET_INIT,
	A52_ACKFR_USB_GADGET_ADD_RC,
	A52_ACKFR_USB_GS_PROBE_RC,
	A52_ACKFR_USB_UDC_BIND_RC,
	A52_ACKFR_USB_PULLUP_RC,
};

struct a52_ackfr_usbdiag_snapshot {
	s32 mode_in;
	s32 hw_mode;
	s32 mode_out;
	s32 core_mode;
	s32 gadget_init;
	s32 gadget_add_rc;
	s32 gs_probe_rc;
	s32 udc_bind_rc;
	s32 pullup_rc;
};

void a52_ackfr_usbdiag_set(unsigned int field, int value);
void a52_ackfr_usbdiag_snapshot(struct a52_ackfr_usbdiag_snapshot *out);
void a52_ackfr_usbdiag_emit(unsigned int sample);


/* A52_PHASE383_EVENT_FRONTIER_LOOP_AIO_V1 */
void a52_ackfr_frontier_trigger(unsigned int loop_no);
u64 a52_ackfr_frontier_trigger_ns(void);
u64 a52_ackfr_frontier_elapsed_ms(void);


/* A52_PHASE385_FIXED_STICKY_V1 */
void a52_ackfr_sticky385_usb_identity(const char *udc, const char *gadget,
                                      const char *parent, const char *driver,
                                      unsigned int pullup_capable);
void a52_ackfr_sticky385_heartbeat(unsigned int sample, u64 elapsed_ms);
void a52_ackfr_sticky385_freeze(unsigned int event, int slot, u64 q,
                                int depth, unsigned int nr,
                                unsigned int zero, u64 age_ms);
void a52_ackfr_sticky385_ufs(unsigned int sample, u32 doorbell,
                             unsigned long outstanding, u64 irq_count);
void a52_ackfr_sticky385_ofsimple(unsigned int stage, int ret);



/* A52_PHASE432_CTRL_ODSIGN_KEYMINT_QSEE_V1 */
void a52_p432ctrl_exec_result(const char *path, long rc);
void a52_p432ctrl_task_exit(long code);

#endif

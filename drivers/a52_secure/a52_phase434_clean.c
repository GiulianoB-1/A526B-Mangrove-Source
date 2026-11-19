// SPDX-License-Identifier: GPL-2.0-only
/* A52_PHASE434_CLEAN_F0_V1
 *
 * Tiny event-driven shared state for the clean SurfaceFlinger/F0 experiment.
 * No polling threads and no remote task inspection.
 */
#include <linux/a52_ack_secure_flight_recorder.h>
#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#define A52_P434_SF_WINDOW_NS (10ULL * NSEC_PER_SEC)

static atomic64_t a52_p434_sf_ns = ATOMIC64_INIT(0);

struct a52_p434_rail_state {
	const char *name;
	int enabled;
	int uv;
	int users;
	u64 ns;
};

static struct a52_p434_rail_state a52_p434_rails[] = {
	{ "pm6350_s5_level", -1, -1, -1, 0 },
	{ "pm6350_l22",      -1, -1, -1, 0 },
	{ "refgen",          -1, -1, -1, 0 },
	{ "panel_vci",       -1, -1, -1, 0 },
	{ "panel_vddi",      -1, -1, -1, 0 },
	{ "mdss_core_gdsc",  -1, -1, -1, 0 },
};
static DEFINE_SPINLOCK(a52_p434_rail_lock);

void a52_p434_note_exec_comm(const char *name, bool exec)
{
	u64 ns;

	if (!exec || !name)
		return;
	ns = ktime_get_boottime_ns();
	if (!strcmp(name, "surfaceflinger")) {
		atomic64_cmpxchg(&a52_p434_sf_ns, 0, (s64)ns);
		a52_ackfr_record("P434 SF EXEC p=%d t=%d ns=%llu",
			current->pid, current->tgid, (unsigned long long)ns);
	} else if (!strcmp(name, "bootanimation")) {
		a52_ackfr_record("P434 BA EXEC p=%d t=%d ns=%llu",
			current->pid, current->tgid, (unsigned long long)ns);
	}
}
EXPORT_SYMBOL_GPL(a52_p434_note_exec_comm);

bool a52_p434_sf_window_active(void)
{
	u64 start = (u64)atomic64_read(&a52_p434_sf_ns);
	u64 now;

	if (!start)
		return false;
	now = ktime_get_boottime_ns();
	return now >= start && now - start <= A52_P434_SF_WINDOW_NS;
}
EXPORT_SYMBOL_GPL(a52_p434_sf_window_active);

void a52_p434_rail_event(const char *name, int enabled, int uv, int users)
{
	unsigned long flags;
	unsigned int i;
	u64 ns;

	if (!name)
		return;
	for (i = 0; i < ARRAY_SIZE(a52_p434_rails); i++)
		if (!strcmp(name, a52_p434_rails[i].name))
			break;
	if (i == ARRAY_SIZE(a52_p434_rails))
		return;

	ns = ktime_get_boottime_ns();
	spin_lock_irqsave(&a52_p434_rail_lock, flags);
	if (enabled >= 0)
		a52_p434_rails[i].enabled = enabled;
	if (uv >= 0)
		a52_p434_rails[i].uv = uv;
	if (users >= 0)
		a52_p434_rails[i].users = users;
	a52_p434_rails[i].ns = ns;
	spin_unlock_irqrestore(&a52_p434_rail_lock, flags);

	a52_ackfr_record("P434 RAIL i=%u en=%d uv=%d u=%d ns=%llu",
		i, enabled, uv, users, (unsigned long long)ns);
}
EXPORT_SYMBOL_GPL(a52_p434_rail_event);

void a52_p434_dump_rails(const char *tag)
{
	struct a52_p434_rail_state s;
	unsigned long flags;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(a52_p434_rails); i++) {
		spin_lock_irqsave(&a52_p434_rail_lock, flags);
		s = a52_p434_rails[i];
		spin_unlock_irqrestore(&a52_p434_rail_lock, flags);
		a52_ackfr_record("P434 RS %.6s i=%u en=%d uv=%d u=%d age=%llu",
			tag ? tag : "-", i, s.enabled, s.uv, s.users,
			s.ns ? (unsigned long long)(ktime_get_boottime_ns() - s.ns) : 0ULL);
	}
}
EXPORT_SYMBOL_GPL(a52_p434_dump_rails);

static int __init a52_p434_init(void)
{
	a52_ackfr_record("P434 BOOT ns=%llu",
		(unsigned long long)ktime_get_boottime_ns());
	return 0;
}
core_initcall(a52_p434_init);

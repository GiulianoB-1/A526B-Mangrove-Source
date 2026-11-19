// SPDX-License-Identifier: GPL-2.0-only
/* A52_PHASE432_CTRL_ODSIGN_KEYMINT_QSEE_V1
 * Passive boot-gate process tracker for the A52 GKI control image.
 * Records only process identity class/timing/exit status. No argv, Binder
 * payload, QSEECOM buffers, keys, certificates, or key payloads are copied.
 */
#include <linux/a52_ack_secure_flight_recorder.h>
#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#define A52_P432C_TRACK_SLOTS 96U

enum a52_p432c_kind {
	A52_P432C_NONE = 0,
	A52_P432C_ODSIGN = 1,
	A52_P432C_ODREFRESH = 2,
	A52_P432C_DEX2OAT = 3,
	A52_P432C_KEYSTORE2 = 4,
	A52_P432C_KEYMINT = 5,
	A52_P432C_QSEECOMD = 6,
	A52_P432C_VDC = 7,
};

struct a52_p432c_slot {
	pid_t tgid;
	u32 seq;
	u32 kind;
	u64 start_ns;
	bool live;
};

static struct a52_p432c_slot a52_p432c_slots[A52_P432C_TRACK_SLOTS];
static DEFINE_SPINLOCK(a52_p432c_lock);
static atomic_t a52_p432c_seq = ATOMIC_INIT(0);

static const char *a52_p432c_base(const char *path)
{
	const char *p;

	if (!path)
		return NULL;
	p = strrchr(path, '/');
	return p ? p + 1 : path;
}

static u32 a52_p432c_classify(const char *path)
{
	const char *b = a52_p432c_base(path);

	if (!b || !*b)
		return A52_P432C_NONE;
	if (!strcmp(b, "odsign"))
		return A52_P432C_ODSIGN;
	if (!strcmp(b, "odrefresh"))
		return A52_P432C_ODREFRESH;
	if (!strncmp(b, "dex2oat", 7))
		return A52_P432C_DEX2OAT;
	if (!strcmp(b, "keystore2"))
		return A52_P432C_KEYSTORE2;
	if (strstr(b, "keymint") || strstr(b, "keymaster"))
		return A52_P432C_KEYMINT;
	if (!strcmp(b, "qseecomd"))
		return A52_P432C_QSEECOMD;
	if (!strcmp(b, "vdc"))
		return A52_P432C_VDC;
	return A52_P432C_NONE;
}

void a52_p432ctrl_exec_result(const char *path, long rc)
{
	struct a52_p432c_slot *slot = NULL;
	struct a52_p432c_slot *free_slot = NULL;
	unsigned long flags;
	u32 kind, seq = 0;
	u64 ns;
	unsigned int i;

	kind = a52_p432c_classify(path);
	if (!kind)
		return;
	ns = ktime_get_boottime_ns();

	if (rc) {
		a52_ackfr_record("P432 C PR F k=%u n=0 p=%d t=%d c=%.15s rc=%ld ns=%llu",
			kind, current->pid, current->tgid, current->comm, rc,
			(unsigned long long)ns);
		return;
	}

	seq = (u32)atomic_inc_return(&a52_p432c_seq);
	spin_lock_irqsave(&a52_p432c_lock, flags);
	for (i = 0; i < A52_P432C_TRACK_SLOTS; i++) {
		if (a52_p432c_slots[i].live &&
		    a52_p432c_slots[i].tgid == current->tgid) {
			slot = &a52_p432c_slots[i];
			break;
		}
		if (!a52_p432c_slots[i].live && !free_slot)
			free_slot = &a52_p432c_slots[i];
	}
	if (!slot)
		slot = free_slot;
	if (slot) {
		slot->tgid = current->tgid;
		slot->seq = seq;
		slot->kind = kind;
		slot->start_ns = ns;
		slot->live = true;
	}
	spin_unlock_irqrestore(&a52_p432c_lock, flags);

	a52_ackfr_record("P432 C PR S k=%u n=%u p=%d t=%d c=%.15s ns=%llu",
		kind, seq, current->pid, current->tgid, current->comm,
		(unsigned long long)ns);
	if (!slot)
		a52_ackfr_record("P432 C PR O k=%u n=%u t=%d ns=%llu",
			kind, seq, current->tgid, (unsigned long long)ns);
}
EXPORT_SYMBOL_GPL(a52_p432ctrl_exec_result);

void a52_p432ctrl_task_exit(long code)
{
	struct a52_p432c_slot found = { };
	unsigned long flags;
	u64 ns;
	unsigned int i;

	spin_lock_irqsave(&a52_p432c_lock, flags);
	for (i = 0; i < A52_P432C_TRACK_SLOTS; i++) {
		if (a52_p432c_slots[i].live &&
		    a52_p432c_slots[i].tgid == current->tgid) {
			found = a52_p432c_slots[i];
			a52_p432c_slots[i].live = false;
			break;
		}
	}
	spin_unlock_irqrestore(&a52_p432c_lock, flags);
	if (!found.live)
		return;

	ns = ktime_get_boottime_ns();
	a52_ackfr_record("P432 C PR X k=%u n=%u p=%d t=%d c=%.15s code=%ld ns=%llu",
		found.kind, found.seq, current->pid, current->tgid, current->comm,
		code, (unsigned long long)ns);
}
EXPORT_SYMBOL_GPL(a52_p432ctrl_task_exit);

/* Keep the names embedded in the image for offline audit. */
static const char * const a52_p432c_names[] __maybe_unused = {
	"none", "odsign", "odrefresh", "dex2oat", "keystore2",
	"keymint", "qseecomd", "vdc",
};

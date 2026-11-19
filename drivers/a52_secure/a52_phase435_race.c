// SPDX-License-Identifier: GPL-2.0-only
/* A52_PHASE435_TERMINAL_RACE_V1
 *
 * Lock-independent, write-once fixed-slot race recorder.  It exists only to
 * order the PID1 fatal-exit lane against the SurfaceFlinger VFS/KGSL-open lane.
 * No polling, remote task inspection, hardware MMIO, regulator, clock or DSI
 * operation is performed here.
 */
#include <linux/a52_ack_secure_flight_recorder.h>
#include <linux/a52_phase435_race.h>
#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <asm/cacheflush.h>
#include <asm/ptrace.h>

#define A52_P435_PHYS       0xB1BFC000ULL
#define A52_P435_BYTES      0x3000U
#define A52_P435_COPY_BYTES 0x1800U
#define A52_P435_SLOT_BYTES 128U
#define A52_P435_SLOTS      48U
#define A52_P435_MAGIC      0x3533344543415254ULL /* TRACE435 */
#define A52_P435_COMMIT     0x435c0de5U
#define A52_P435_VERSION    1U

struct a52_p435_record {
	u64 magic;
	u64 boot_id;
	u64 boottime_ns;
	u64 arch_counter;
	u64 arg0;
	u64 arg1;
	u64 pc;
	u64 lr;
	u64 sp;
	u64 pstate;
	u32 version;
	u32 stage;
	u32 cpu;
	u32 pid;
	u32 tgid;
	u32 slot;
	char comm[TASK_COMM_LEN];
	u32 crc32c;
	u32 commit;
} __packed;

static void *a52_p435_base;
static atomic_t a52_p435_written[A52_P435_SLOTS];
static atomic_t a52_p435_target_pid = ATOMIC_INIT(-1);
static atomic_t a52_p435_target_tgid = ATOMIC_INIT(-1);
static u64 a52_p435_id;

static __always_inline u64 a52_p435_counter(void)
{
	u64 v;
	asm volatile("isb; mrs %0, cntpct_el0" : "=r" (v));
	return v;
}


static u32 a52_p435_crc32c(const void *buffer, size_t len)
{
	const u8 *bytes = buffer;
	u32 crc = ~0U;
	size_t i;
	unsigned int bit;

	for (i = 0; i < len; i++) {
		crc ^= bytes[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1U) ? 0x82f63b78U : 0U);
	}
	return ~crc;
}

static void a52_p435_write_copy(void *dst, const struct a52_p435_record *r)
{
	struct a52_p435_record *out = dst;

	/* Payload + CRC first; commit is the final persistent word. */
	memcpy(out, r, offsetof(struct a52_p435_record, commit));
	wmb();
	WRITE_ONCE(out->commit, A52_P435_COMMIT);
	wmb();
	__flush_dcache_area(out, sizeof(*out));
	dsb(sy);
}

void a52_p435_mark(u32 stage, u64 arg0, u64 arg1,
		   const struct pt_regs *regs)
{
	struct a52_p435_record r;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_p435_base) || stage >= A52_P435_SLOTS)
		return;
	if (atomic_cmpxchg(&a52_p435_written[stage], 0, 1))
		return;

	memset(&r, 0, sizeof(r));
	r.magic = A52_P435_MAGIC;
	r.boot_id = READ_ONCE(a52_p435_id);
	r.boottime_ns = ktime_get_boottime_ns();
	r.arch_counter = a52_p435_counter();
	r.arg0 = arg0;
	r.arg1 = arg1;
	if (regs) {
		r.pc = regs->pc;
		r.lr = regs->regs[30];
		r.sp = regs->sp;
		r.pstate = regs->pstate;
	}
	r.version = A52_P435_VERSION;
	r.stage = stage;
	r.cpu = raw_smp_processor_id();
	r.pid = current->pid;
	r.tgid = current->tgid;
	r.slot = stage;
	memcpy(r.comm, current->comm, TASK_COMM_LEN);
	r.crc32c = a52_p435_crc32c(&r, offsetof(struct a52_p435_record, crc32c));
	r.commit = 0;

	dst0 = (u8 *)a52_p435_base + stage * A52_P435_SLOT_BYTES;
	dst1 = (u8 *)a52_p435_base + A52_P435_COPY_BYTES +
		stage * A52_P435_SLOT_BYTES;
	a52_p435_write_copy(dst0, &r);
	a52_p435_write_copy(dst1, &r);
}
EXPORT_SYMBOL_GPL(a52_p435_mark);

void a52_p435_arm_target(u64 arg0, u64 arg1)
{
	if (strcmp(current->comm, "surfaceflinger"))
		return;
	atomic_cmpxchg(&a52_p435_target_pid, -1, current->pid);
	atomic_cmpxchg(&a52_p435_target_tgid, -1, current->tgid);
	if (atomic_read(&a52_p435_target_pid) == current->pid &&
	    atomic_read(&a52_p435_target_tgid) == current->tgid)
		a52_p435_mark(P435_K0_TARGET, arg0, arg1, NULL);
}
EXPORT_SYMBOL_GPL(a52_p435_arm_target);

bool a52_p435_target_current(void)
{
	return atomic_read(&a52_p435_target_pid) == current->pid &&
	       atomic_read(&a52_p435_target_tgid) == current->tgid;
}
EXPORT_SYMBOL_GPL(a52_p435_target_current);

u64 a52_p435_boot_id(void)
{
	return READ_ONCE(a52_p435_id);
}
EXPORT_SYMBOL_GPL(a52_p435_boot_id);

static int __init a52_p435_init(void)
{
	/* Phase436 proved the old B1BFC000 range is active ramoops/pmsg. */
	a52_p435_base = NULL;
	a52_ackfr_record("P435 RETIRED pmsg_overlap=1");
	return 0;
}
core_initcall(a52_p435_init);

/* A52_PHASE436_RETIRE_P435_PMSG_WRITER_V1 */

// SPDX-License-Identifier: GPL-2.0-only
/* A52_PHASE436_ADRENO_START_FRONTIER_V1
 *
 * Fixed-slot, lock-independent terminal-path recorder for the first
 * SurfaceFlinger KGSL start. No polling, remote task inspection or hardware
 * register reads are performed. The recorder occupies the final 16 KiB of the
 * already collector-visible Phase414 reserved RAM window; Phase414 is shrunk
 * by exactly the same 16 KiB so the mappings do not overlap.
 */
#include <linux/a52_ack_secure_flight_recorder.h>
#include <linux/a52_phase436_frontier.h>
#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <asm/cacheflush.h>
#include <asm/ptrace.h>

#define A52_P436_PHYS       0xB1AFC000ULL
#define A52_P436_BYTES      0x4000U
#define A52_P436_COPY_BYTES 0x2000U
#define A52_P436_SLOT_BYTES 128U
#define A52_P436_SLOTS      64U
#define A52_P436_MAGIC      0x3633344543415254ULL /* TRACE436 */
#define A52_P436_COMMIT     0x436c0de5U
#define A52_P436_VERSION    1U

struct a52_p436_record {
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

static void *a52_p436_base;
static atomic_t a52_p436_written[A52_P436_SLOTS];
static atomic_t a52_p436_target_pid = ATOMIC_INIT(-1);
static atomic_t a52_p436_target_tgid = ATOMIC_INIT(-1);
static u64 a52_p436_id;

static __always_inline u64 a52_p436_counter(void)
{
	u64 v;
	asm volatile("isb; mrs %0, cntpct_el0" : "=r" (v));
	return v;
}

static __always_inline u64 a52_p436_counter_freq(void)
{
	u64 v;
	asm volatile("mrs %0, cntfrq_el0" : "=r" (v));
	return v;
}

static u32 a52_p436_crc32c(const void *buffer, size_t len)
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

static void a52_p436_write_copy(void *dst, const struct a52_p436_record *r)
{
	struct a52_p436_record *out = dst;

	memcpy(out, r, offsetof(struct a52_p436_record, commit));
	wmb();
	WRITE_ONCE(out->commit, A52_P436_COMMIT);
	wmb();
	__flush_dcache_area(out, sizeof(*out));
	dsb(sy);
}

void a52_p436_mark(u32 stage, u64 arg0, u64 arg1,
		   const struct pt_regs *regs)
{
	struct a52_p436_record r;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_p436_base) || stage >= A52_P436_SLOTS)
		return;
	if (atomic_cmpxchg(&a52_p436_written[stage], 0, 1))
		return;

	memset(&r, 0, sizeof(r));
	r.magic = A52_P436_MAGIC;
	r.boot_id = READ_ONCE(a52_p436_id);
	r.boottime_ns = ktime_get_boottime_ns();
	r.arch_counter = a52_p436_counter();
	r.arg0 = arg0;
	r.arg1 = arg1;
	if (regs) {
		r.pc = regs->pc;
		r.lr = regs->regs[30];
		r.sp = regs->sp;
		r.pstate = regs->pstate;
	}
	r.version = A52_P436_VERSION;
	r.stage = stage;
	r.cpu = raw_smp_processor_id();
	r.pid = current->pid;
	r.tgid = current->tgid;
	r.slot = stage;
	memcpy(r.comm, current->comm, TASK_COMM_LEN);
	r.crc32c = a52_p436_crc32c(&r,
		offsetof(struct a52_p436_record, crc32c));
	r.commit = 0;

	dst0 = (u8 *)a52_p436_base + stage * A52_P436_SLOT_BYTES;
	dst1 = (u8 *)a52_p436_base + A52_P436_COPY_BYTES +
		stage * A52_P436_SLOT_BYTES;
	a52_p436_write_copy(dst0, &r);
	a52_p436_write_copy(dst1, &r);
}
EXPORT_SYMBOL_GPL(a52_p436_mark);

void a52_p436_arm_target(void)
{
	if (!current->group_leader ||
	    strcmp(current->group_leader->comm, "surfaceflinger"))
		return;
	atomic_cmpxchg(&a52_p436_target_pid, -1, current->pid);
	atomic_cmpxchg(&a52_p436_target_tgid, -1, current->tgid);
}
EXPORT_SYMBOL_GPL(a52_p436_arm_target);

bool a52_p436_target_current(void)
{
	return atomic_read(&a52_p436_target_pid) == current->pid &&
	       atomic_read(&a52_p436_target_tgid) == current->tgid;
}
EXPORT_SYMBOL_GPL(a52_p436_target_current);

u64 a52_p436_boot_id(void)
{
	return READ_ONCE(a52_p436_id);
}
EXPORT_SYMBOL_GPL(a52_p436_boot_id);

static int __init a52_p436_init(void)
{
	u64 counter;
	u64 freq;
	unsigned int i;

	BUILD_BUG_ON(sizeof(struct a52_p436_record) != A52_P436_SLOT_BYTES);
	BUILD_BUG_ON(A52_P436_SLOTS * A52_P436_SLOT_BYTES != A52_P436_COPY_BYTES);

	a52_p436_base = memremap(A52_P436_PHYS, A52_P436_BYTES, MEMREMAP_WB);
	if (!a52_p436_base)
		return 0;
	memset(a52_p436_base, 0, A52_P436_BYTES);
	for (i = 0; i < A52_P436_SLOTS; i++)
		atomic_set(&a52_p436_written[i], 0);
	wmb();
	__flush_dcache_area(a52_p436_base, A52_P436_BYTES);
	dsb(sy);

	counter = a52_p436_counter();
	freq = a52_p436_counter_freq();
	a52_p436_id = counter ^ (ktime_get_boottime_ns() << 1) ^
		(u64)(unsigned long)a52_p436_init;
	if (!a52_p436_id)
		a52_p436_id = 1;

	a52_p436_mark(P436_META, freq, A52_P436_PHYS, NULL);
	a52_ackfr_record("P436 BOOT id=%llx phys=%llx bytes=%x frq=%llu",
		(unsigned long long)a52_p436_id,
		(unsigned long long)A52_P436_PHYS, A52_P436_BYTES,
		(unsigned long long)freq);
	return 0;
}
core_initcall(a52_p436_init);

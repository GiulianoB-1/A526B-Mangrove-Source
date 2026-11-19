#include <linux/rcupdate.h>
extern void a52_p440_event(const char *kind, const char *name, long a, long b);
#include <linux/a52_ack_secure_flight_recorder.h>
#include <linux/sched/task_stack.h>
#include <linux/kallsyms.h>
#include <linux/stacktrace.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched/signal.h>
// SPDX-License-Identifier: GPL-2.0

#include <linux/compiler.h>
#include <linux/context_tracking.h>
#include <linux/errno.h>
#include <linux/nospec.h>
#include <linux/ptrace.h>
#include <linux/syscalls.h>
#include <linux/build_bug.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/futex.h>
#include <asm/cacheflush.h>

#include <asm/daifflags.h>
#include <asm/debug-monitors.h>
#include <asm/fpsimd.h>
#include <asm/syscall.h>
#include <asm/thread_info.h>
#include <asm/unistd.h>

#include <linux/a52_phase435_race.h>
#include <linux/a52_phase436_frontier.h>
long compat_arm_syscall(struct pt_regs *regs, int scno);
long sys_ni_syscall(void);

/* A52_PHASE358_PID1_SYSCALL_TRACE_V1
 *
 * Phase357 proved that PID1 reaches do_group_exit()/do_exit() with 0x7f00,
 * then hits the global-init panic. Phase358 moves one layer earlier and keeps
 * the last 32 arm64 syscalls issued by PID1 in ordinary RAM. No persistent
 * cache maintenance is done during normal operation.
 *
 * When PID1 enters exit(2) or exit_group(2), the 32-entry history is written
 * in chronological order to the Phase357 32 KiB sideband and the final syscall
 * snapshot is replicated 32 times. This minimizes perturbation before failure
 * while retaining strong resistance to pmsg clear-bit damage.
 *
 * Phase358 owns 0xB1BF0000..0xB1BF7FFF. Phase357's mapper is deliberately
 * disabled by this phase, while its source and panic instrumentation remain
 * compiled for lineage.
 */
#define A52_R358_SIDEBAND_PHYS    0xB1BF0000ULL
#define A52_R358_SIDEBAND_BYTES   0x8000U
#define A52_R358_COPY_BYTES       0x4000U
#define A52_R358_SLOT_BYTES       256U
#define A52_R358_HISTORY_SLOTS    32U
#define A52_R358_FINAL_FIRST      32U
#define A52_R358_SLOTS_PER_COPY   64U
#define A52_R358_MAGIC            0x3835335449584550ULL /* "PEXIT358" */
#define A52_R358_COMMIT           0x358c0de5U
#define A52_R358_VERSION          1U

#define A52_R358_EVT_ENTRY        1U
#define A52_R358_EVT_RETURN       2U
#define A52_R358_EVT_FINAL_EXIT   3U

struct a52_r358_record {
	u64 magic;
	u64 ns;
	u64 sequence;
	u64 pc;
	u64 sp;
	u64 pstate;
	u64 fp;
	u64 lr;
	u64 x[9];
	u64 mm;
	u64 start_code;
	u64 end_code;
	u64 start_data;
	u64 end_data;
	u64 start_stack;
	s64 ret;
	u32 event;
	u32 syscallno;
	u32 cpu;
	u32 pid;
	u32 tgid;
	u32 ring_index;
	u32 commit;
	u32 version;
	char comm[TASK_COMM_LEN];
	u8 reserved[16];
};


/* A52_PHASE359_PID1_WRITEV_PAYLOAD_V1
 *
 * Phase358 proved PID1 reaches Bionic abort() after a first-stage-mount
 * timeout. Phase359 captures the last four PID1 writev payloads so the
 * exact Android init error text and missing partition/device names survive.
 *
 * Normal writev calls update only a four-entry RAM ring. On PID1
 * exit/exit_group, each of the four records is replicated four times in each
 * 16 KiB mirror of the 32 KiB Phase358 sideband.
 */
#define A52_R359_SIDEBAND_PHYS    0xB1BF0000ULL
#define A52_R359_SIDEBAND_BYTES   0x8000U
#define A52_R359_COPY_BYTES       0x4000U
#define A52_R359_SLOT_BYTES       1024U
#define A52_R359_RING_SLOTS       4U
#define A52_R359_REPLICAS         4U
#define A52_R359_SLOTS_PER_COPY   16U
#define A52_R359_MAGIC            0x3935335654495257ULL /* "WRITV359" */
#define A52_R359_COMMIT           0x359c0de5U
#define A52_R359_VERSION          1U

#define A52_R359_EVT_ENTRY        1U
#define A52_R359_EVT_RETURN       2U

struct a52_r359_record {
	u64 magic;
	u64 ns;
	u64 sequence;
	u64 pc;
	u64 lr;
	u64 sp;
	s64 ret;
	u32 event;
	s32 fd;
	u32 iovcnt;
	u32 payload_len;
	u32 cpu;
	u32 pid;
	u32 tgid;
	u32 ring_index;
	u32 commit;
	u32 version;
	char comm[TASK_COMM_LEN];
	u8 payload[912];
};


/* A52_PHASE364_APEXD_SYSCALL_RING_V1
 *
 * Phase363 hardware ran to the 330 s safety window. UFS is no longer the
 * frontier, but the second /system/bin/apexd group leader starts around 18.6 s
 * and never exits; odsign, zygote and SurfaceFlinger never start.
 *
 * Reuse the now-idle Phase359 32 KiB sideband. Trace only an apexd group
 * leader (pid == tgid, comm == "apexd"). Each syscall entry/return is persisted
 * immediately into a 64-slot circular ring mirrored in two 16 KiB copies.
 * An unmatched final ENTRY therefore identifies a blocking syscall directly.
 *
 * No syscall arguments, return values, scheduling or Android behavior are
 * changed.
 */
#define A52_R364_SIDEBAND_PHYS    0xB1BF0000ULL
#define A52_R364_SIDEBAND_BYTES   0x8000U
#define A52_R364_COPY_BYTES       0x4000U
#define A52_R364_SLOT_BYTES       256U
#define A52_R364_SLOTS            64U
#define A52_R364_MAGIC            0x3436335845504141ULL /* "AAPEX364" LE-ish */
#define A52_R364_COMMIT           0x364c0de5U
#define A52_R364_VERSION          1U

#define A52_R364_EVT_ENTRY        1U
#define A52_R364_EVT_RETURN       2U

struct a52_r364_record {
	u64 magic;
	u64 ns;
	u64 sequence;
	u64 pc;
	u64 sp;
	u64 pstate;
	u64 lr;
	u64 x[6];
	s64 ret;
	u32 event;
	u32 syscallno;
	u32 cpu;
	u32 pid;
	u32 tgid;
	u32 ring_index;
	u32 commit;
	u32 version;
	char comm[TASK_COMM_LEN];
	u8 reserved[96];
};


/* A52_PHASE365_APEXD_WRITEV_FUTEX_V1
 *
 * Phase364 hardware caught the second apexd group leader entering
 * futex(FUTEX_WAIT_BITSET_PRIVATE) at ~19.035167649 s and never returning.
 * The immediately preceding group-leader syscall (~27.5 us earlier) is writev.
 *
 * Reuse the Phase364 32 KiB sideband as a 16-entry, 1 KiB circular event ring
 * mirrored into two 16 KiB copies. Trace only the apexd group leader.
 *
 * WRITEV records preserve up to 844 payload bytes. FUTEX records preserve all
 * six syscall arguments. A blocking futex remains as an ENTRY record with the
 * INT64_MIN return sentinel. Returned calls are rewritten in-place and flushed.
 */
#define A52_R365_SIDEBAND_PHYS    0xB1BF0000ULL
#define A52_R365_SIDEBAND_BYTES   0x8000U
#define A52_R365_COPY_BYTES       0x4000U
#define A52_R365_SLOT_BYTES       1024U
#define A52_R365_SLOTS            16U
#define A52_R365_PAYLOAD_BYTES    844U
#define A52_R365_MAGIC            0x3536335746585041ULL
#define A52_R365_COMMIT           0x365c0de5U
#define A52_R365_VERSION          1U

#define A52_R365_EVT_WRITEV_ENTRY  1U
#define A52_R365_EVT_WRITEV_RETURN 2U
#define A52_R365_EVT_FUTEX_ENTRY   3U
#define A52_R365_EVT_FUTEX_RETURN  4U

struct a52_r365_record {
	u64 magic;
	u64 ns;
	u64 sequence;
	u64 pc;
	u64 lr;
	u64 sp;
	u64 x[6];
	s64 ret;
	u32 event;
	u32 syscallno;
	u32 cpu;
	u32 pid;
	u32 tgid;
	u32 ring_index;
	u32 commit;
	u32 version;
	s32 fd;
	u32 iovcnt;
	u32 payload_len;
	char comm[TASK_COMM_LEN];
	u8 payload[A52_R365_PAYLOAD_BYTES];
	u8 reserved[16];
};


/* A52_PHASE366_APEXD_FUTEX_THREADS_V1
 *
 * Phase365 recovered the apexd group leader's property-service writev:
 * PROP_MSG_SETPROP2("apexd.status", "starting"). The same process later enters
 * a permanent FUTEX_WAIT_BITSET_PRIVATE before apexd.status reaches activated
 * or ready.
 *
 * Trace futex activity from every thread whose group leader is "apexd".
 * Each call occupies one 256-byte slot and is rewritten in-place on return.
 * The latest group-leader FUTEX_WAIT* call is additionally replicated into
 * eight reserved slots, so the final blocking wait survives worker traffic.
 *
 * No futex arguments, scheduling, return values, userspace memory or Android
 * behavior are modified.
 */
#define A52_R366_SIDEBAND_PHYS      0xB1BF0000ULL
#define A52_R366_SIDEBAND_BYTES     0x8000U
#define A52_R366_COPY_BYTES         0x4000U
#define A52_R366_SLOT_BYTES         256U
#define A52_R366_RING_SLOTS         56U
#define A52_R366_LATCH_FIRST        56U
#define A52_R366_LATCH_REPLICAS     8U
#define A52_R366_SLOTS_PER_COPY     64U
#define A52_R366_MAGIC              0x3636335854554641ULL
#define A52_R366_COMMIT             0x366c0de5U
#define A52_R366_VERSION            1U

#define A52_R366_EVT_ENTRY          1U
#define A52_R366_EVT_RETURN         2U

struct a52_r366_record {
	u64 magic;
	u64 ns;
	u64 sequence;
	u64 uaddr;
	u64 val;
	u64 timeout;
	u64 uaddr2;
	u64 val3;
	u64 target_uaddr;
	s64 ret;
	u32 event;
	u32 syscallno;
	u32 op;
	u32 cpu;
	u32 pid;
	u32 tgid;
	u32 ring_index;
	u32 commit;
	u32 version;
	char comm[TASK_COMM_LEN];
	u8 reserved[120];
};


/* A52_PHASE368_APEXD_WORKER_SYSCALL_MAP_V1
 *
 * Phase367 proved the apexd group leader was successfully woken from one
 * FUTEX_WAIT_BITSET_PRIVATE, then immediately blocked on a second futex that
 * was never targeted by a later wake. That strongly deprioritizes a generic
 * futex-wake failure and instead points to an async APEX activation worker
 * that never reaches completion.
 *
 * Phase368 persists the latest syscall state independently for every thread
 * belonging to the apexd process. Each thread owns one of 64 mirrored slots.
 * Syscall ENTRY overwrites the slot with ret=LONG_MIN; normal RETURN rewrites
 * the same slot with the actual result. A thread stuck inside the kernel
 * therefore survives as an ENTRY record, while a thread that exited normally
 * is marked explicitly as EXIT_ENTRY.
 *
 * Observation only: syscall arguments, return values, userspace memory,
 * scheduling and kernel behavior are not modified.
 */
#define A52_R368_SIDEBAND_PHYS      0xB1BF0000ULL
#define A52_R368_SIDEBAND_BYTES     0x8000U
#define A52_R368_COPY_BYTES         0x4000U
#define A52_R368_SLOT_BYTES         256U
#define A52_R368_SLOTS              64U
#define A52_R368_MAGIC              0x3836335853504141ULL
#define A52_R368_COMMIT             0x368c0de5U
#define A52_R368_VERSION            1U

#define A52_R368_EVT_ENTRY          1U
#define A52_R368_EVT_RETURN         2U
#define A52_R368_EVT_EXIT_ENTRY     3U

#define A52_R368_PAYLOAD_NONE       0U
#define A52_R368_PAYLOAD_PATH_X0    1U
#define A52_R368_PAYLOAD_PATH_X1    2U
#define A52_R368_PAYLOAD_MOUNT      3U
#define A52_R368_PAYLOAD_RENAME     4U

struct a52_r368_record {
	u64 magic;
	u64 ns;
	u64 sequence;
	u64 pc;
	u64 lr;
	u64 sp;
	u64 clear_child_tid;
	u64 mm;
	u64 x[6];
	s64 ret;
	u32 event;
	u32 syscallno;
	u32 cpu;
	u32 pid;
	u32 tgid;
	u32 slot_index;
	u32 commit;
	u32 version;
	u32 payload_len;
	u32 payload_kind;
	char comm[TASK_COMM_LEN];
	u8 payload[80];
};


/* A52_PHASE369_POST_APEX_FRONTIER_V1
 *
 * Phase368 hardware result:
 * - bootstrap apexd exited normally
 * - the later apexd activation workers captured by the per-thread map exited
 * - the later apexd group leader remained alive at the Phase339 safety cutoff
 *   in syscall 101 (__NR_nanosleep)
 * - inherited Phase346 DSI raw slots p0..p6 were still empty
 *
 * Phase369 answers two questions in one boot:
 * 1) Why is the main apexd sleeping after activation?  Record a chronological
 *    group-leader syscall ring, including nanosleep duration and writev/path
 *    payloads.
 * 2) How far did init continue after apexd activation?  Record execve/execveat
 *    attempts for direct PID1 children in a second ring.
 *
 * 32 KiB sideband, mirrored 16 KiB copies.  Each copy contains:
 * slots  0..31: apexd group-leader syscall chronology
 * slots 32..63: direct-init-child exec chronology
 *
 * Observation only.
 */
#define A52_R369_SIDEBAND_PHYS      0xB1BF0000ULL
#define A52_R369_SIDEBAND_BYTES     0x8000U
#define A52_R369_COPY_BYTES         0x4000U
#define A52_R369_SLOT_BYTES         256U
#define A52_R369_SLOTS_PER_COPY     64U
#define A52_R369_MAIN_FIRST         0U
#define A52_R369_MAIN_SLOTS         32U
#define A52_R369_EXEC_FIRST         32U
#define A52_R369_EXEC_SLOTS         32U
#define A52_R369_MAGIC              0x3936335846504141ULL
#define A52_R369_COMMIT             0x369c0de5U
#define A52_R369_VERSION            1U

#define A52_R369_EVT_MAIN_ENTRY     1U
#define A52_R369_EVT_MAIN_RETURN    2U
#define A52_R369_EVT_INIT_EXEC      10U

#define A52_R369_PAYLOAD_NONE       0U
#define A52_R369_PAYLOAD_WRITEV     1U
#define A52_R369_PAYLOAD_PATH       2U

struct a52_r369_record {
	u64 magic;
	u64 ns;
	u64 sequence;
	u64 pc;
	u64 lr;
	u64 sp;
	u64 mm;
	u64 x[6];
	s64 ret;
	s64 sleep_sec;
	s64 sleep_nsec;
	u32 event;
	u32 syscallno;
	u32 cpu;
	u32 pid;
	u32 tgid;
	u32 ppid;
	u32 ring_index;
	u32 commit;
	u32 version;
	u32 payload_len;
	u32 payload_kind;
	char comm[TASK_COMM_LEN];
	u8 payload[68];
};


/* A52_PHASE370_LATEST_APEX_WORKER_V1
 *
 * Phase369 hardware result:
 * - later apexd group leader PID 487 repeatedly created async workers
 * - clone() returned child TID 536 immediately before the final blocked future
 * - main then completed a short FUTEX_WAIT_BITSET_PRIVATE and entered another
 *   FUTEX_WAIT_BITSET_PRIVATE on 0xb400007d7b24f690 that never returned
 * - no zygote or SurfaceFlinger direct-init exec appeared, and Phase346 DSI
 *   p0..p6 remained empty
 *
 * Phase370 latches the most recently cloned apexd worker. Each successful
 * group-leader clone/clone3 return replaces the target PID and resets the
 * target-worker sequence. Only syscalls from that exact PID/TGID are then
 * recorded. Four header replicas preserve the latest clone target; slots 4..63
 * are a 60-entry mirrored chronological syscall ring.
 *
 * Observation only: no syscall arguments, return values, scheduling, futex
 * behavior, userspace memory or APEX logic are modified.
 */
#define A52_R370_SIDEBAND_PHYS      0xB1BF0000ULL
#define A52_R370_SIDEBAND_BYTES     0x8000U
#define A52_R370_COPY_BYTES         0x4000U
#define A52_R370_SLOT_BYTES         256U
#define A52_R370_SLOTS_PER_COPY     64U
#define A52_R370_HEADER_SLOTS       4U
#define A52_R370_RING_FIRST         4U
#define A52_R370_RING_SLOTS         60U
#define A52_R370_MAGIC              0x3037335857504141ULL
#define A52_R370_COMMIT             0x370c0de5U
#define A52_R370_VERSION            1U

#define A52_R370_EVT_TARGET         1U
#define A52_R370_EVT_ENTRY          2U
#define A52_R370_EVT_RETURN         3U
#define A52_R370_EVT_EXIT_ENTRY     4U

#define A52_R370_PAYLOAD_NONE       0U
#define A52_R370_PAYLOAD_PATH_X0    1U
#define A52_R370_PAYLOAD_PATH_X1    2U
#define A52_R370_PAYLOAD_MOUNT      3U
#define A52_R370_PAYLOAD_WRITEV     4U

struct a52_r370_record {
	u64 magic;
	u64 ns;
	u64 sequence;
	u64 target_pid;
	u64 target_tgid;
	u64 pc;
	u64 lr;
	u64 sp;
	u64 mm;
	u64 clear_child_tid;
	u64 x[6];
	s64 ret;
	u32 event;
	u32 syscallno;
	u32 cpu;
	u32 pid;
	u32 tgid;
	u32 ppid;
	u32 ring_index;
	u32 commit;
	u32 version;
	u32 payload_len;
	u32 payload_kind;
	char comm[TASK_COMM_LEN];
	u8 payload[60];
};


/* A52_PHASE371_THREAD_WORKER_STATE_V1
 *
 * Phase370 hardware result exposed a targeting bug in the recorder itself.
 * The final latched clone returned PID 1062 with flags 0x01200011:
 *   CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD
 * and therefore was a separate child process, not an apexd pthread worker.
 * Phase370 assigned current->tgid to every clone target and required that TGID
 * when matching, guaranteeing zero records for such a process child.
 *
 * Phase371 fixes target selection by latching only legacy clone() calls with
 * CLONE_THREAD set. This matches the Phase369 async-worker pattern
 * (clone flags around 0x003d0f00).
 *
 * In addition to the target worker's syscall chronology, a sampler records the
 * target task's scheduler/runtime state once per second. This distinguishes:
 *   - runnable but never scheduled
 *   - actively consuming CPU in userspace
 *   - sleeping/blocked task
 *   - exited/disappeared task
 *
 * Sideband layout per 16 KiB mirror:
 * slots  0..3  : replicated latest CLONE_THREAD target header
 * slots  4..31 : 28-entry target-worker syscall ring
 * slots 32..63 : 32-entry target-task snapshot ring
 *
 * Observation only.
 */
#define A52_R371_SIDEBAND_PHYS      0xB1BF0000ULL
#define A52_R371_SIDEBAND_BYTES     0x8000U
#define A52_R371_COPY_BYTES         0x4000U
#define A52_R371_SLOT_BYTES         256U
#define A52_R371_SLOTS_PER_COPY     64U
#define A52_R371_HEADER_SLOTS       4U
#define A52_R371_SYSCALL_FIRST      4U
#define A52_R371_SYSCALL_SLOTS      28U
#define A52_R371_SNAPSHOT_FIRST     32U
#define A52_R371_SNAPSHOT_SLOTS     32U
#define A52_R371_MAGIC              0x3137335857504141ULL
#define A52_R371_COMMIT             0x371c0de5U
#define A52_R371_VERSION            1U

#define A52_R371_EVT_TARGET         1U
#define A52_R371_EVT_ENTRY          2U
#define A52_R371_EVT_RETURN         3U
#define A52_R371_EVT_EXIT_ENTRY     4U
#define A52_R371_EVT_SNAPSHOT       5U
#define A52_R371_EVT_MISSING        6U

struct a52_r371_record {
	u64 magic;
	u64 ns;
	u64 sequence;
	u64 target_pid;
	u64 target_tgid;
	u64 clone_flags;
	u64 task_state;
	u64 task_flags;
	u64 sum_exec_runtime;
	u64 nvcsw;
	u64 nivcsw;
	u64 clear_child_tid;
	u64 mm;
	u64 x[6];
	s64 ret;
	u32 event;
	u32 syscallno;
	u32 cpu;
	u32 pid;
	u32 tgid;
	u32 ppid;
	u32 exit_state;
	u32 on_cpu;
	u32 on_rq;
	u32 task_cpu;
	u32 ring_index;
	u32 commit;
	u32 version;
	char comm[TASK_COMM_LEN];
	u8 reserved[28];
};


/* A52_PHASE372_FUTURE_WORKER_CORRELATION_V1
 *
 * Phase371 corrected Phase370's CLONE_THREAD targeting and proved that the
 * latest real apexd worker (TID 537 in TGID 489) executed substantial APEX
 * work and then exited normally about 202 ms after creation.
 *
 * That result also proves "latest worker" is the wrong correlation target:
 * apexd first launches all std::async workers, stores their futures, and only
 * then waits on futures in vector order. The main thread can therefore block
 * on an early worker while later-created workers have already completed.
 *
 * Phase372 correlates the three things we actually need:
 *   1. direct apexd-main CLONE_THREAD creation order
 *   2. apexd-main FUTEX_WAIT/FUTEX_WAIT_BITSET chronology
 *   3. per-worker terminal state and FUTEX_WAKE activity
 *
 * Sideband per 16 KiB mirror, 128-byte records:
 *   slots   0..15  main apexd futex-wait ring
 *   slots  16..71  56 persistent worker summaries by clone ordinal
 *   slots  72..127 56-entry worker clone/wake/exit event ring
 *
 * A worker summary survives later traffic and records its last syscall,
 * last futex uaddr/op/val, wake return/count, exit state, clear_child_tid,
 * and whether it ever successfully woke the exact main wait target active
 * at that moment. This lets us identify the worker/future ordering without
 * assuming PID adjacency or "latest clone == current future".
 *
 * Observation only.
 */
#define A52_R372_SIDEBAND_PHYS      0xB1BF0000ULL
#define A52_R372_SIDEBAND_BYTES     0x8000U
#define A52_R372_COPY_BYTES         0x4000U
#define A52_R372_SLOT_BYTES         128U
#define A52_R372_SLOTS_PER_COPY     128U
#define A52_R372_MAIN_FIRST         0U
#define A52_R372_MAIN_SLOTS         16U
#define A52_R372_WORKER_FIRST       16U
#define A52_R372_WORKER_SLOTS       56U
#define A52_R372_EVENT_FIRST        72U
#define A52_R372_EVENT_SLOTS        56U
#define A52_R372_MAGIC              0x3237335857434141ULL
#define A52_R372_COMMIT             0x372c0de5U
#define A52_R372_VERSION            1U

#define A52_R372_F_CLONED           BIT(0)
#define A52_R372_F_SYSCALL_ENTRY    BIT(1)
#define A52_R372_F_SYSCALL_RETURN   BIT(2)
#define A52_R372_F_WAKE             BIT(3)
#define A52_R372_F_WAKE_MATCH       BIT(4)
#define A52_R372_F_EXIT             BIT(5)
#define A52_R372_F_MAIN_WAIT_ENTRY  BIT(8)
#define A52_R372_F_MAIN_WAIT_RETURN BIT(9)

struct a52_r372_record {
	u64 magic;
	u64 first_ns;
	u64 last_ns;
	u64 uaddr;
	u64 clear_child_tid;
	s64 last_ret;
	u32 event_flags;
	u32 ordinal;
	u32 pid;
	u32 tgid;
	u32 clone_flags;
	u32 last_syscall;
	u32 futex_op;
	u32 futex_val;
	s32 last_wake_ret;
	u32 wake_calls;
	u32 matched_wait_seq;
	u32 exit_seen;
	u32 exit_code;
	u32 commit;
	u32 version;
	char comm[TASK_COMM_LEN];
	u8 reserved[4];
};


/* A52_PHASE373_APEX_MOUNT_BLOCKER_V1
 *
 * Phase372 hardware result:
 * - second apexd process TGID 493 begins at worker ordinal 49 because the first
 *   apexd consumed ordinals 0..48.
 * - worker 49/PID 496 successfully wakes main futex wait #5.
 * - workers 50..54 have all exited before main advances.
 * - worker 55/PID 502 enters syscall 40 (__NR_mount) at ~18.972 s and never
 *   returns before the ~330 s safety reboot.
 * - main returns from wait #5 at ~19.065165 s and only ~47.9 us later enters
 *   wait #6 forever. The blocked mount had already been outstanding ~92.8 ms.
 *
 * Phase373 resets worker ordinals for each new apexd TGID, so the problematic
 * worker is expected near local ordinal 6 instead of global ordinal 55.
 *
 * Each worker gets one persistent 512-byte record containing its latest mount:
 * source, target, filesystem type, flags, return state, and (after 100 ms
 * blocked) a kernel task stack snapshot.  The top stack address is also
 * symbolized in-kernel using sprint_symbol().
 *
 * 32 records per 16 KiB mirror; worker ordinal maps directly to slot.
 * Observation only.
 */
#define A52_R373_SIDEBAND_PHYS      0xB1BF0000ULL
#define A52_R373_SIDEBAND_BYTES     0x8000U
#define A52_R373_COPY_BYTES         0x4000U
#define A52_R373_SLOT_BYTES         512U
#define A52_R373_SLOTS_PER_COPY     32U
#define A52_R373_MAX_WORKERS        32U
#define A52_R373_STACK_ENTRIES      12U
#define A52_R373_MAGIC              0x333733584d414141ULL
#define A52_R373_COMMIT             0x373c0de5U
#define A52_R373_VERSION            1U

#define A52_R373_EVT_MOUNT_ENTRY    1U
#define A52_R373_EVT_MOUNT_RETURN   2U
#define A52_R373_EVT_STACK_SAMPLE   3U

struct a52_r373_record {
	u64 magic;
	u64 entry_ns;
	u64 sample_ns;
	u64 return_ns;
	s64 ret;
	u64 mount_flags;
	u32 generation;
	u32 ordinal;
	u32 pid;
	u32 tgid;
	u32 event;
	u32 task_state;
	u32 stack_nr;
	u32 cpu;
	u32 commit;
	u32 version;
	char comm[TASK_COMM_LEN];
	char source[80];
	char target[120];
	char fstype[32];
	unsigned long stack[A52_R373_STACK_ENTRIES];
	char top_symbol[64];
	u8 reserved[16];
};


/* A52_PHASE377_APEXD_LIVE_THREAD_CENSUS_V1
 *
 * Phase373 hardware result:
 * - the Phase372 suspect maps to local worker ordinal 6 in the later apexd
 * - its final mount is the APEX active-path bind mount for
 *   com.samsung.android.camera.unihal
 * - mount flags are 0x1000 (MS_BIND), source is the versioned /apex path,
 *   target is /apex/com.samsung.android.camera.unihal
 * - in this boot the mount has a RETURN record with ret=0 in both mirrors
 * - every surviving Phase373 mount record is a RETURN; no persistent
 *   MOUNT_ENTRY/STACK_SAMPLE remains without a return
 *
 * Therefore the previous mount stall is timing-sensitive or the blocker moved.
 * Phase377 takes direct live thread-group censuses at 19, 20, 25 and 40 seconds
 * rather than assuming the same syscall will stall again.
 *
 * Per 16 KiB mirror:
 *   4 snapshots x 16 records x 256 bytes = 16 KiB
 * The apexd group leader is always record 0 of each snapshot; up to 15 other
 * live threads follow.  Each record captures task state, wchan + symbol,
 * 6 kernel stack PCs, scheduler/runtime counters and saved userspace PC/LR/SP.
 *
 * Observation only.
 */
#define A52_R377_SIDEBAND_PHYS      0xB1BF0000ULL
#define A52_R377_SIDEBAND_BYTES     0x8000U
#define A52_R377_COPY_BYTES         0x2400U
#define A52_R377_SLOT_BYTES         256U
#define A52_R377_SLOTS_PER_COPY     36U
#define A52_R377_SNAPSHOT_COUNT     4U
#define A52_R377_THREADS_PER_SNAP   9U
#define A52_R377_STACK_ENTRIES      6U
#define A52_R377_MAGIC              0x37373358534e4543ULL
#define A52_R377_COMMIT             0x383c0de5U
#define A52_R377_VERSION            3U

struct a52_r377_record {
	u64 magic;
	u64 ns;
	u64 wchan;
	u64 user_pc;
	u64 user_lr;
	u64 user_sp;
	u64 sum_exec_runtime;
	u64 clear_child_tid;
	unsigned long stack[A52_R377_STACK_ENTRIES];
	u32 snapshot_id;
	u32 pid;
	u32 tgid;
	u32 ppid;
	u32 task_state;
	u32 exit_state;
	u32 task_flags;
	u32 on_cpu;
	u32 on_rq;
	u32 task_cpu;
	u32 nr_threads;
	u32 live_threads;
	u32 stack_nr;
	u32 commit;
	u32 version;
	char comm[TASK_COMM_LEN];
	char wchan_symbol[64];
	u32 crc32c;
};

static const char a52_r377_marker[] __used =
	"A52_PHASE377_APEXD_LIVE_THREAD_CENSUS_V1";

/* A52_PHASE382_CLIFF_CENSUS_V1 */
static u32 a52_r382_crc32c(const void *buffer, size_t len)
{
	const u8 *bytes = buffer;
	u32 crc = ~0U;
	size_t index;
	unsigned int bit;

	for (index = 0; index < len; index++) {
		crc ^= bytes[index];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
				((crc & 1) ? 0x82f63b78U : 0U);
	}
	return ~crc;
}






/* A52_PHASE383_EVENT_CENSUS_V1 */


/* A52_PHASE380_VDC_LIVE_CENSUS_V1
 *
 * The Phase379 hardware capture changed the live frontier: all observed
 * apexd processes exited by ~12.85 s, while the third /system/bin/vdc
 * instance exec'd at ~16.48 s and had no exit record before the protected
 * stream ended at ~17.345 s. Retire the now-obsolete Phase377 apexd census
 * and sample the latest live vdc task from a dedicated kthread.
 *
 * Records go through a52_ackfr_record(), so they inherit the main recorder's
 * RS48 + CRC32C + three-bank persistence.
 */
#define A52_R380_VDC_SNAPSHOT_COUNT 6U
#define A52_R380_VDC_STACK_ENTRIES  8U





/* Phase429 retires the unrelated Phase380 VDC sampler. */

/* Phase380 replaces the Phase377 apexd census at runtime. */



/* A52_PHASE430_SF_DMA_FORENSICS_V1
 *
 * Event-relative userspace display census.  Phase429 proved that the old
 * apexd page-read stall can disappear while the first DRM atomic is still
 * delayed by ~73 s.  Phase430 therefore starts its clock when SurfaceFlinger
 * actually exists and samples the display stack at +5/+20/+40/+60/+70 s,
 * plus one endpoint snapshot when Phase418 observes the first DRM atomic.
 *
 * The 32 KiB Phase429 sideband is retained but upgraded to three independent
 * copies with per-record CRC32C.  Composer identity comes from Phase269's
 * exec-derived TGID, not a truncated comm prefix.
 */
#define A52_P430_TASK_PHYS          0xB1BF0000ULL
#define A52_P430_TASK_BYTES         0x8000U
#define A52_P430_COPY_BYTES         0x2A00U
#define A52_P430_SLOT_BYTES         256U
#define A52_P430_SNAPSHOTS          7U
#define A52_P430_PER_SNAPSHOT       6U
#define A52_P430_SLOTS              42U
#define A52_P430_STACK              8U
#define A52_P430_MAGIC              0x303334534e454353ULL
#define A52_P430_COMMIT             0x430c0de5U
#define A52_P430_VERSION            1U
#define A52_P430_ROLE_SF            2U
#define A52_P430_ROLE_COMPOSER      3U
#define A52_P430_ROLE_SYSTEM        4U
#define A52_P432_ROLE_BOOTANIM       5U

struct a52_p430_task_record {
	u64 magic;
	u64 ns;
	u64 wchan;
	u64 user_pc;
	u64 user_lr;
	u64 user_sp;
	u64 sum_exec_runtime;
	unsigned long stack[A52_P430_STACK];
	u32 snapshot_id;
	u32 role;
	u32 pid;
	u32 tgid;
	u32 ppid;
	u32 task_state;
	u32 exit_state;
	u32 task_flags;
	u32 on_cpu;
	u32 on_rq;
	u32 task_cpu;
	u32 stack_nr;
	u32 nr_threads;
	u32 live_threads;
	u32 nvcsw;
	u32 nivcsw;
	u32 slot;
	char comm[TASK_COMM_LEN];
	char wchan_symbol[32];
	u8 reserved[8];
	u32 crc32c;
	u32 commit;
	u32 version;
};

static void *a52_p430_task_sideband;
static atomic_t a52_p430_first_atomic = ATOMIC_INIT(0);
static atomic_t a52_p430_atomic_tgid = ATOMIC_INIT(-1);
static u64 a52_p430_sf_start_ns;


/* A52_PHASE431_SAFE_SF_GAP_FORENSICS_V1
 *
 * The final 0x200 bytes of the Phase430 task sideband are unused once the
 * three task-copy strides are widened to 0x2a00.  Use that tail for one
 * three-copy execution witness.  It is driven by raw CNTPCT reads during
 * short CPU7 busy-execution windows, not by timer callbacks.
 */
#define A52_P431_WITNESS_OFF         0x7e00U
#define A52_P431_WITNESS_BYTES       0x0100U
#define A52_P431_WITNESS_STRIDE      0x0050U
#define A52_P431_WITNESS_MAGIC       0x3133345745584543ULL
#define A52_P431_WITNESS_COMMIT      0x431c0de5U
#define A52_P431_WITNESS_VERSION     1U
#define A52_P431_EXEC_WINDOWS        16U
#define A52_P431_EXEC_START_MS       4500ULL
#define A52_P431_EXEC_PERIOD_MS      1000ULL
#define A52_P431_EXEC_BUSY_MS        800ULL

#define A52_P432_FRONTIER_OFF        0x7f00U
#define A52_P432_FRONTIER_BYTES      0x0100U
#define A52_P432_FRONTIER_STRIDE     0x0050U
#define A52_P432_F_SF_SEEN           0x21U
#define A52_P432_F_BOOTANIM_SEEN     0x23U

struct a52_p431_witness {
	u64 magic;
	u64 cntpct;
	u64 sf_start_ns;
	u64 loops;
	u64 write_ns;
	u32 sequence;
	u32 sequence_inv;
	u32 cpu;
	u32 stage;
	u32 snapshot_id;
	u32 cntfrq;
	u32 crc32c;
	u32 commit;
	u32 version;
	u32 reserved;
};

static DEFINE_RAW_SPINLOCK(a52_p431_witness_lock);
static atomic_t a52_p431_witness_seq = ATOMIC_INIT(0);

bool a52_p431_trace_window_active(void)
{
	u64 start = READ_ONCE(a52_p430_sf_start_ns);
	u64 now;

	if (!start)
		return false;
	now = ktime_get_boottime_ns();
	return now >= start && now - start <= 70ULL * NSEC_PER_SEC;
}
EXPORT_SYMBOL_GPL(a52_p431_trace_window_active);

static __always_inline u64 a52_p431_cntpct(void)
{
	u64 v;
	asm volatile("isb; mrs %0, cntpct_el0" : "=r" (v));
	return v;
}

static __always_inline u32 a52_p431_cntfrq(void)
{
	u64 v;
	asm volatile("mrs %0, cntfrq_el0" : "=r" (v));
	return (u32)v;
}


void a52_p432_frontier(u32 stage, pid_t tgid, u32 aux)
{
	struct a52_p431_witness w;
	unsigned long flags;
	void *d0, *d1, *d2;
	u32 seq;

	if (!READ_ONCE(a52_p430_task_sideband))
		return;
	memset(&w, 0, sizeof(w));
	seq = (u32)atomic_inc_return(&a52_p431_witness_seq);
	w.magic = A52_P431_WITNESS_MAGIC;
	w.cntpct = a52_p431_cntpct();
	w.sf_start_ns = READ_ONCE(a52_p430_sf_start_ns);
	w.loops = (u64)(u32)tgid;
	w.write_ns = ktime_get_boottime_ns();
	w.sequence = seq;
	w.sequence_inv = ~seq;
	w.cpu = (u32)raw_smp_processor_id();
	w.stage = stage;
	w.snapshot_id = aux;
	w.cntfrq = a52_p431_cntfrq();
	w.crc32c = a52_r382_crc32c(&w, offsetof(struct a52_p431_witness, crc32c));
	w.commit = A52_P431_WITNESS_COMMIT;
	w.version = A52_P431_WITNESS_VERSION;

	raw_spin_lock_irqsave(&a52_p431_witness_lock, flags);
	d0 = (u8 *)a52_p430_task_sideband + A52_P432_FRONTIER_OFF;
	d1 = (u8 *)d0 + A52_P432_FRONTIER_STRIDE;
	d2 = (u8 *)d1 + A52_P432_FRONTIER_STRIDE;
	memcpy(d0, &w, sizeof(w));
	memcpy(d1, &w, sizeof(w));
	memcpy(d2, &w, sizeof(w));
	wmb();
	__flush_dcache_area(d0, sizeof(w));
	__flush_dcache_area(d1, sizeof(w));
	__flush_dcache_area(d2, sizeof(w));
	dsb(sy);
	raw_spin_unlock_irqrestore(&a52_p431_witness_lock, flags);
}
EXPORT_SYMBOL_GPL(a52_p432_frontier);


extern bool a52_ackfr_phase269_is_composer_tgid(pid_t tgid);
extern int a52_ackfr_phase432_drm_owner_tgid(void);

void a52_p430_note_first_atomic(pid_t tgid)
{
	if (atomic_cmpxchg(&a52_p430_first_atomic, 0, 1) == 0) {
		atomic_set(&a52_p430_atomic_tgid, (int)tgid);
		a52_ackfr_record("P432 AT0 t=%d ns=%llu", tgid,
			(unsigned long long)ktime_get_boottime_ns());
	}
}
EXPORT_SYMBOL_GPL(a52_p430_note_first_atomic);












static int __init a52_p430_init(void)
{
	a52_p430_task_sideband = memremap(A52_P430_TASK_PHYS,
		A52_P430_TASK_BYTES, MEMREMAP_WB);
	if (!a52_p430_task_sideband)
		return 0;
	memset(a52_p430_task_sideband, 0, A52_P430_TASK_BYTES);
	wmb();
	__flush_dcache_area(a52_p430_task_sideband, A52_P430_TASK_BYTES);
	dsb(sy);
	a52_ackfr_record("P434 CLEAN sampler=0 remote_task=0");
	return 0;
}
late_initcall(a52_p430_init);

static const char a52_r373_marker[] __used =
	"A52_PHASE373_APEX_MOUNT_BLOCKER_V1";
static void *a52_r373_sideband;
static u32 a52_r373_generation;
static u32 a52_r373_tgid;
static u32 a52_r373_worker_count;
static u32 a52_r373_worker_pid[A52_R373_MAX_WORKERS];
static struct a52_r373_record a52_r373_records[A52_R373_MAX_WORKERS];

static bool a52_r373_apexd_group_leader(void)
{
	return current->pid == current->tgid &&
	       !strncmp(current->comm, "apexd", TASK_COMM_LEN);
}

static void a52_r373_write_slot(unsigned int slot,
				const struct a52_r373_record *r)
{
	unsigned int pos;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r373_sideband) || !r ||
	    slot >= A52_R373_SLOTS_PER_COPY)
		return;

	pos = slot * A52_R373_SLOT_BYTES;
	dst0 = (u8 *)a52_r373_sideband + pos;
	dst1 = (u8 *)a52_r373_sideband + A52_R373_COPY_BYTES + pos;
	memcpy(dst0, r, sizeof(*r));
	memcpy(dst1, r, sizeof(*r));
	wmb();
	__flush_dcache_area(dst0, sizeof(*r));
	__flush_dcache_area(dst1, sizeof(*r));
}

static void a52_r373_copy_string(char *dst, size_t cap,
				 const char __user *src)
{
	long n;

	if (!dst || !cap || !src)
		return;
	n = strncpy_from_user(dst, src, cap);
	if (n < 0)
		return;
	dst[cap - 1] = '\0';
}

static void a52_r373_reset_generation(u32 tgid)
{
	WRITE_ONCE(a52_r373_tgid, tgid);
	WRITE_ONCE(a52_r373_worker_count, 0);
	WRITE_ONCE(a52_r373_generation, READ_ONCE(a52_r373_generation) + 1U);
	memset(a52_r373_worker_pid, 0, sizeof(a52_r373_worker_pid));
	memset(a52_r373_records, 0, sizeof(a52_r373_records));
	memset(a52_r373_sideband, 0, A52_R373_SIDEBAND_BYTES);
	wmb();
	__flush_dcache_area(a52_r373_sideband, A52_R373_SIDEBAND_BYTES);
}

static void a52_r373_note_clone_return(struct pt_regs *regs, int scno)
{
	u64 flags;
	s64 child;
	u32 ordinal;

	if (!READ_ONCE(a52_r373_sideband) || !a52_r373_apexd_group_leader() ||
	    scno != __NR_clone)
		return;

	flags = regs->orig_x0;
	if (!(flags & CLONE_THREAD))
		return;

	child = (s64)regs->regs[0];
	if (child <= 0 || child > INT_MAX)
		return;

	if (READ_ONCE(a52_r373_tgid) != (u32)current->tgid)
		a52_r373_reset_generation((u32)current->tgid);

	ordinal = READ_ONCE(a52_r373_worker_count);
	if (ordinal >= A52_R373_MAX_WORKERS)
		return;

	WRITE_ONCE(a52_r373_worker_pid[ordinal], (u32)child);
	WRITE_ONCE(a52_r373_worker_count, ordinal + 1U);
}

static int a52_r373_find_ordinal(void)
{
	u32 i;
	u32 count;

	if ((u32)current->tgid != READ_ONCE(a52_r373_tgid))
		return -1;

	count = min_t(u32, READ_ONCE(a52_r373_worker_count),
		      A52_R373_MAX_WORKERS);
	for (i = 0; i < count; i++)
		if (READ_ONCE(a52_r373_worker_pid[i]) == (u32)current->pid)
			return (int)i;
	return -1;
}

static u64 a52_r373_mount_enter(struct pt_regs *regs, int scno)
{
	struct a52_r373_record *r;
	int ordinal;

	if (!READ_ONCE(a52_r373_sideband) || scno != __NR_mount ||
	    current->pid == current->tgid)
		return 0;

	ordinal = a52_r373_find_ordinal();
	if (ordinal < 0)
		return 0;

	r = &a52_r373_records[ordinal];
	memset(r, 0, sizeof(*r));
	r->magic = A52_R373_MAGIC;
	r->entry_ns = ktime_get_ns();
	r->ret = (s64)(1ULL << 63);
	r->mount_flags = regs->regs[3];
	r->generation = READ_ONCE(a52_r373_generation);
	r->ordinal = (u32)ordinal;
	r->pid = (u32)current->pid;
	r->tgid = (u32)current->tgid;
	r->event = A52_R373_EVT_MOUNT_ENTRY;
	r->cpu = (u32)raw_smp_processor_id();
	r->commit = A52_R373_COMMIT;
	r->version = A52_R373_VERSION;
	get_task_comm(r->comm, current);

	a52_r373_copy_string(r->source, sizeof(r->source),
		(const char __user *)(unsigned long)regs->orig_x0);
	a52_r373_copy_string(r->target, sizeof(r->target),
		(const char __user *)(unsigned long)regs->regs[1]);
	a52_r373_copy_string(r->fstype, sizeof(r->fstype),
		(const char __user *)(unsigned long)regs->regs[2]);

	a52_r373_write_slot((unsigned int)ordinal, r);
	return (u64)ordinal + 1ULL;
}

static void a52_r373_mount_return(struct pt_regs *regs, int scno, u64 token)
{
	struct a52_r373_record *r;
	unsigned int ordinal;

	if (!token || !READ_ONCE(a52_r373_sideband) || scno != __NR_mount)
		return;

	ordinal = (unsigned int)(token - 1ULL);
	if (ordinal >= A52_R373_MAX_WORKERS)
		return;

	r = &a52_r373_records[ordinal];
	if (r->pid != (u32)current->pid || r->tgid != (u32)current->tgid)
		return;

	r->return_ns = ktime_get_ns();
	r->ret = (s64)regs->regs[0];
	r->event = A52_R373_EVT_MOUNT_RETURN;
	r->cpu = (u32)raw_smp_processor_id();
	a52_r373_write_slot(ordinal, r);
}



/* Phase377 owns 0xB1BF0000..0xB1BF7FFF at runtime. */

static const char a52_r372_marker[] __used =
	"A52_PHASE372_FUTURE_WORKER_CORRELATION_V1";
static void *a52_r372_sideband;
static atomic_t a52_r372_worker_count = ATOMIC_INIT(0);
static atomic64_t a52_r372_main_wait_sequence = ATOMIC64_INIT(0);
static atomic64_t a52_r372_event_sequence = ATOMIC64_INIT(0);
static u32 a52_r372_worker_pid[A52_R372_WORKER_SLOTS];
static u32 a52_r372_worker_tgid[A52_R372_WORKER_SLOTS];
static struct a52_r372_record
	a52_r372_worker_summary[A52_R372_WORKER_SLOTS];
static u64 a52_r372_main_wait_uaddr;
static u32 a52_r372_main_wait_seq;

static bool a52_r372_apexd_group_leader(void)
{
	return current->pid == current->tgid &&
	       !strncmp(current->comm, "apexd", TASK_COMM_LEN);
}

static bool a52_r372_apexd_thread(void)
{
	char comm[TASK_COMM_LEN];

	if (!current->group_leader)
		return false;
	get_task_comm(comm, current->group_leader);
	return !strncmp(comm, "apexd", TASK_COMM_LEN);
}

static void a52_r372_write_slot(unsigned int slot,
				const struct a52_r372_record *r)
{
	unsigned int pos;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r372_sideband) || !r ||
	    slot >= A52_R372_SLOTS_PER_COPY)
		return;

	pos = slot * A52_R372_SLOT_BYTES;
	dst0 = (u8 *)a52_r372_sideband + pos;
	dst1 = (u8 *)a52_r372_sideband + A52_R372_COPY_BYTES + pos;
	memcpy(dst0, r, sizeof(*r));
	memcpy(dst1, r, sizeof(*r));
	wmb();
	__flush_dcache_area(dst0, sizeof(*r));
	__flush_dcache_area(dst1, sizeof(*r));
}

static void a52_r372_fill_identity(struct a52_r372_record *r)
{
	r->pid = (u32)current->pid;
	r->tgid = (u32)current->tgid;
	r->clear_child_tid =
		(u64)(unsigned long)READ_ONCE(current->clear_child_tid);
	get_task_comm(r->comm, current);
}

static void a52_r372_emit_event(const struct a52_r372_record *src,
				u32 event_flags)
{
	struct a52_r372_record r;
	u64 sequence;
	unsigned int slot;

	if (!src || !READ_ONCE(a52_r372_sideband))
		return;

	sequence = (u64)atomic64_inc_return(&a52_r372_event_sequence);
	slot = A52_R372_EVENT_FIRST +
	       (unsigned int)((sequence - 1ULL) % A52_R372_EVENT_SLOTS);
	r = *src;
	r.event_flags = event_flags;
	r.last_ns = ktime_get_ns();
	r.ordinal = src->ordinal;
	a52_r372_write_slot(slot, &r);
}

static int a52_r372_find_worker(u32 pid, u32 tgid)
{
	unsigned int i;
	unsigned int count = min_t(unsigned int,
				   (unsigned int)atomic_read(&a52_r372_worker_count),
				   A52_R372_WORKER_SLOTS);

	for (i = 0; i < count; i++) {
		if (READ_ONCE(a52_r372_worker_pid[i]) == pid &&
		    READ_ONCE(a52_r372_worker_tgid[i]) == tgid)
			return (int)i;
	}
	return -1;
}

static void a52_r372_note_clone_return(struct pt_regs *regs, int scno)
{
	struct a52_r372_record *r;
	s64 child = (s64)regs->regs[0];
	u64 flags;
	int ordinal;

	if (!READ_ONCE(a52_r372_sideband) || !a52_r372_apexd_group_leader())
		return;
	if (scno != __NR_clone)
		return;

	flags = regs->orig_x0;
	if (!(flags & CLONE_THREAD))
		return;
	if (child <= 0 || child > INT_MAX)
		return;

	ordinal = atomic_inc_return(&a52_r372_worker_count) - 1;
	if (ordinal < 0 || ordinal >= A52_R372_WORKER_SLOTS)
		return;

	WRITE_ONCE(a52_r372_worker_pid[ordinal], (u32)child);
	WRITE_ONCE(a52_r372_worker_tgid[ordinal], (u32)current->tgid);

	r = &a52_r372_worker_summary[ordinal];
	memset(r, 0, sizeof(*r));
	r->magic = A52_R372_MAGIC;
	r->first_ns = ktime_get_ns();
	r->last_ns = r->first_ns;
	r->last_ret = child;
	r->event_flags = A52_R372_F_CLONED;
	r->ordinal = (u32)ordinal;
	r->pid = (u32)child;
	r->tgid = (u32)current->tgid;
	r->clone_flags = (u32)flags;
	r->last_syscall = (u32)scno;
	r->commit = A52_R372_COMMIT;
	r->version = A52_R372_VERSION;
	memcpy(r->comm, "apexd", 6);

	a52_r372_write_slot(A52_R372_WORKER_FIRST + (unsigned int)ordinal, r);
	a52_r372_emit_event(r, A52_R372_F_CLONED);
}

static u64 a52_r372_main_wait_enter(struct pt_regs *regs, int scno)
{
	struct a52_r372_record r;
	u64 sequence;
	u32 op;
	u32 cmd;
	unsigned int slot;

	if (!READ_ONCE(a52_r372_sideband) || !a52_r372_apexd_group_leader() ||
	    scno != __NR_futex)
		return 0;

	op = (u32)regs->regs[1];
	cmd = op & FUTEX_CMD_MASK;
	if (cmd != FUTEX_WAIT && cmd != FUTEX_WAIT_BITSET)
		return 0;

	sequence = (u64)atomic64_inc_return(&a52_r372_main_wait_sequence);
	slot = A52_R372_MAIN_FIRST +
	       (unsigned int)((sequence - 1ULL) % A52_R372_MAIN_SLOTS);

	memset(&r, 0, sizeof(r));
	r.magic = A52_R372_MAGIC;
	r.first_ns = ktime_get_ns();
	r.last_ns = r.first_ns;
	r.uaddr = regs->orig_x0;
	r.clear_child_tid =
		(u64)(unsigned long)READ_ONCE(current->clear_child_tid);
	r.last_ret = (s64)(1ULL << 63);
	r.event_flags = A52_R372_F_MAIN_WAIT_ENTRY;
	r.ordinal = (u32)sequence;
	r.pid = (u32)current->pid;
	r.tgid = (u32)current->tgid;
	r.last_syscall = (u32)scno;
	r.futex_op = op;
	r.futex_val = (u32)regs->regs[2];
	r.commit = A52_R372_COMMIT;
	r.version = A52_R372_VERSION;
	get_task_comm(r.comm, current);
	a52_r372_write_slot(slot, &r);

	WRITE_ONCE(a52_r372_main_wait_uaddr, r.uaddr);
	WRITE_ONCE(a52_r372_main_wait_seq, (u32)sequence);
	return sequence;
}

static void a52_r372_main_wait_return(struct pt_regs *regs, int scno,
				      u64 sequence)
{
	struct a52_r372_record r;
	unsigned int slot;

	if (!sequence || !READ_ONCE(a52_r372_sideband) ||
	    !a52_r372_apexd_group_leader() || scno != __NR_futex)
		return;

	slot = A52_R372_MAIN_FIRST +
	       (unsigned int)((sequence - 1ULL) % A52_R372_MAIN_SLOTS);

	memset(&r, 0, sizeof(r));
	r.magic = A52_R372_MAGIC;
	r.first_ns = 0;
	r.last_ns = ktime_get_ns();
	r.uaddr = regs->orig_x0;
	r.clear_child_tid =
		(u64)(unsigned long)READ_ONCE(current->clear_child_tid);
	r.last_ret = (s64)regs->regs[0];
	r.event_flags = A52_R372_F_MAIN_WAIT_RETURN;
	r.ordinal = (u32)sequence;
	r.pid = (u32)current->pid;
	r.tgid = (u32)current->tgid;
	r.last_syscall = (u32)scno;
	r.futex_op = (u32)regs->regs[1];
	r.futex_val = (u32)regs->regs[2];
	r.commit = A52_R372_COMMIT;
	r.version = A52_R372_VERSION;
	get_task_comm(r.comm, current);
	a52_r372_write_slot(slot, &r);

	if (READ_ONCE(a52_r372_main_wait_seq) == (u32)sequence) {
		WRITE_ONCE(a52_r372_main_wait_uaddr, 0);
		WRITE_ONCE(a52_r372_main_wait_seq, 0);
	}
}

static void a52_r372_worker_enter(struct pt_regs *regs, int scno)
{
	struct a52_r372_record *r;
	int ordinal;

	if (!READ_ONCE(a52_r372_sideband) || !a52_r372_apexd_thread() ||
	    current->pid == current->tgid)
		return;

	ordinal = a52_r372_find_worker((u32)current->pid, (u32)current->tgid);
	if (ordinal < 0)
		return;

	r = &a52_r372_worker_summary[ordinal];
	r->last_ns = ktime_get_ns();
	r->last_syscall = (u32)scno;
	r->last_ret = (s64)(1ULL << 63);
	r->event_flags |= A52_R372_F_SYSCALL_ENTRY;
	a52_r372_fill_identity(r);

	if (scno == __NR_futex) {
		r->uaddr = regs->orig_x0;
		r->futex_op = (u32)regs->regs[1];
		r->futex_val = (u32)regs->regs[2];
	}

	if (scno == __NR_exit || scno == __NR_exit_group) {
		r->event_flags |= A52_R372_F_EXIT;
		r->exit_seen = 1;
		r->exit_code = (u32)regs->orig_x0;
		a52_r372_write_slot(A52_R372_WORKER_FIRST +
				    (unsigned int)ordinal, r);
		a52_r372_emit_event(r, A52_R372_F_EXIT);
		return;
	}

	a52_r372_write_slot(A52_R372_WORKER_FIRST + (unsigned int)ordinal, r);
}

static void a52_r372_worker_return(struct pt_regs *regs, int scno)
{
	struct a52_r372_record *r;
	u32 cmd = ~0U;
	s32 wake_ret;
	int ordinal;

	if (!READ_ONCE(a52_r372_sideband) || !a52_r372_apexd_thread() ||
	    current->pid == current->tgid)
		return;

	ordinal = a52_r372_find_worker((u32)current->pid, (u32)current->tgid);
	if (ordinal < 0)
		return;

	r = &a52_r372_worker_summary[ordinal];
	r->last_ns = ktime_get_ns();
	r->last_syscall = (u32)scno;
	r->last_ret = (s64)regs->regs[0];
	r->event_flags |= A52_R372_F_SYSCALL_RETURN;
	a52_r372_fill_identity(r);

	if (scno == __NR_futex) {
		r->uaddr = regs->orig_x0;
		r->futex_op = (u32)regs->regs[1];
		r->futex_val = (u32)regs->regs[2];
		cmd = r->futex_op & FUTEX_CMD_MASK;
	}

	if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
		wake_ret = (s32)regs->regs[0];
		r->event_flags |= A52_R372_F_WAKE;
		r->last_wake_ret = wake_ret;
		r->wake_calls++;

		if (wake_ret > 0 &&
		    r->uaddr == READ_ONCE(a52_r372_main_wait_uaddr) &&
		    READ_ONCE(a52_r372_main_wait_seq)) {
			r->event_flags |= A52_R372_F_WAKE_MATCH;
			if (!r->matched_wait_seq)
				r->matched_wait_seq =
					READ_ONCE(a52_r372_main_wait_seq);
		}

		a52_r372_write_slot(A52_R372_WORKER_FIRST +
				    (unsigned int)ordinal, r);
		a52_r372_emit_event(r,
			(r->event_flags & A52_R372_F_WAKE_MATCH) ?
			A52_R372_F_WAKE | A52_R372_F_WAKE_MATCH :
			A52_R372_F_WAKE);
		return;
	}

	a52_r372_write_slot(A52_R372_WORKER_FIRST + (unsigned int)ordinal, r);
}

static int __init __used a52_r372_init(void)
{
	BUILD_BUG_ON(sizeof(struct a52_r372_record) != A52_R372_SLOT_BYTES);
	BUILD_BUG_ON(A52_R372_SLOTS_PER_COPY * A52_R372_SLOT_BYTES !=
		     A52_R372_COPY_BYTES);
	BUILD_BUG_ON(A52_R372_MAIN_SLOTS + A52_R372_WORKER_SLOTS +
		     A52_R372_EVENT_SLOTS != A52_R372_SLOTS_PER_COPY);

	a52_r372_sideband = memremap(A52_R372_SIDEBAND_PHYS,
		A52_R372_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r372_sideband)
		return 0;

	memset(a52_r372_sideband, 0, A52_R372_SIDEBAND_BYTES);
	memset(a52_r372_worker_pid, 0, sizeof(a52_r372_worker_pid));
	memset(a52_r372_worker_tgid, 0, sizeof(a52_r372_worker_tgid));
	memset(a52_r372_worker_summary, 0, sizeof(a52_r372_worker_summary));
	atomic_set(&a52_r372_worker_count, 0);
	atomic64_set(&a52_r372_main_wait_sequence, 0);
	atomic64_set(&a52_r372_event_sequence, 0);
	WRITE_ONCE(a52_r372_main_wait_uaddr, 0);
	WRITE_ONCE(a52_r372_main_wait_seq, 0);
	wmb();
	__flush_dcache_area(a52_r372_sideband, A52_R372_SIDEBAND_BYTES);
	return 0;
}
/* Phase373 owns 0xB1BF0000..0xB1BF7FFF at runtime. */

static const char a52_r371_marker[] __used =
	"A52_PHASE371_THREAD_WORKER_STATE_V1";
static void *a52_r371_sideband;
static atomic64_t a52_r371_sys_sequence = ATOMIC64_INIT(0);
static atomic64_t a52_r371_snap_sequence = ATOMIC64_INIT(0);
static u32 a52_r371_target_pid;
static u32 a52_r371_target_tgid;
static u64 a52_r371_clone_flags;
static struct task_struct *a52_r371_sampler_task;

static bool a52_r371_apexd_group_leader(void)
{
	return current->pid == current->tgid &&
	       !strncmp(current->comm, "apexd", TASK_COMM_LEN);
}

static bool a52_r371_is_target(void)
{
	u32 pid = READ_ONCE(a52_r371_target_pid);
	u32 tgid = READ_ONCE(a52_r371_target_tgid);

	return pid && current->pid == pid && current->tgid == tgid;
}

static void a52_r371_fill_task(struct a52_r371_record *r,
			       struct task_struct *p)
{
	if (!p)
		return;

	r->task_state = (u64)READ_ONCE(p->state);
	r->task_flags = (u64)READ_ONCE(p->flags);
	r->sum_exec_runtime = READ_ONCE(p->se.sum_exec_runtime);
	r->nvcsw = (u64)READ_ONCE(p->nvcsw);
	r->nivcsw = (u64)READ_ONCE(p->nivcsw);
	r->clear_child_tid =
		(u64)(unsigned long)READ_ONCE(p->clear_child_tid);
	r->mm = (u64)(unsigned long)READ_ONCE(p->mm);
	r->exit_state = (u32)READ_ONCE(p->exit_state);
#ifdef CONFIG_SMP
	r->on_cpu = (u32)READ_ONCE(p->on_cpu);
#else
	r->on_cpu = 0;
#endif
	r->on_rq = (u32)READ_ONCE(p->on_rq);
	r->task_cpu = (u32)task_cpu(p);
	r->pid = (u32)p->pid;
	r->tgid = (u32)p->tgid;
	r->ppid = (u32)task_ppid_nr(p);
	get_task_comm(r->comm, p);
}

static void a52_r371_fill_current(struct a52_r371_record *r,
				  struct pt_regs *regs, int scno,
				  u32 event, u64 sequence, s64 ret,
				  unsigned int slot)
{
	unsigned int i;

	memset(r, 0, sizeof(*r));
	r->magic = A52_R371_MAGIC;
	r->ns = ktime_get_ns();
	r->sequence = sequence;
	r->target_pid = READ_ONCE(a52_r371_target_pid);
	r->target_tgid = READ_ONCE(a52_r371_target_tgid);
	r->clone_flags = READ_ONCE(a52_r371_clone_flags);
	if (regs) {
		r->x[0] = regs->orig_x0;
		for (i = 1; i < ARRAY_SIZE(r->x); i++)
			r->x[i] = regs->regs[i];
	}
	r->ret = ret;
	r->event = event;
	r->syscallno = (u32)scno;
	r->cpu = (u32)raw_smp_processor_id();
	r->ring_index = slot;
	r->commit = A52_R371_COMMIT;
	r->version = A52_R371_VERSION;
	a52_r371_fill_task(r, current);
}

static void a52_r371_write_slot(unsigned int slot,
				const struct a52_r371_record *r)
{
	unsigned int pos;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r371_sideband) || !r ||
	    slot >= A52_R371_SLOTS_PER_COPY)
		return;

	pos = slot * A52_R371_SLOT_BYTES;
	dst0 = (u8 *)a52_r371_sideband + pos;
	dst1 = (u8 *)a52_r371_sideband + A52_R371_COPY_BYTES + pos;
	memcpy(dst0, r, sizeof(*r));
	memcpy(dst1, r, sizeof(*r));
	wmb();
	__flush_dcache_area(dst0, sizeof(*r));
	__flush_dcache_area(dst1, sizeof(*r));
}

static void a52_r371_write_target_header(struct pt_regs *regs, int scno,
					 s64 child, u64 flags)
{
	struct a52_r371_record r;
	unsigned int i;

	a52_r371_fill_current(&r, regs, scno, A52_R371_EVT_TARGET,
			      0, child, 0);
	r.target_pid = (u64)(u32)child;
	r.target_tgid = (u64)(u32)current->tgid;
	r.clone_flags = flags;

	for (i = 0; i < A52_R371_HEADER_SLOTS; i++) {
		r.ring_index = i;
		a52_r371_write_slot(i, &r);
	}
}

static void a52_r371_note_clone_return(struct pt_regs *regs, int scno)
{
	s64 child = (s64)regs->regs[0];
	u64 flags;

	if (!READ_ONCE(a52_r371_sideband) || !a52_r371_apexd_group_leader())
		return;
	if (scno != __NR_clone)
		return;

	flags = regs->orig_x0;
	if (!(flags & CLONE_THREAD))
		return;
	if (child <= 0 || child > INT_MAX)
		return;

	WRITE_ONCE(a52_r371_target_tgid, (u32)current->tgid);
	WRITE_ONCE(a52_r371_target_pid, (u32)child);
	WRITE_ONCE(a52_r371_clone_flags, flags);
	atomic64_set(&a52_r371_sys_sequence, 0);
	atomic64_set(&a52_r371_snap_sequence, 0);
	a52_r371_write_target_header(regs, scno, child, flags);
}

static u64 a52_r371_worker_enter(struct pt_regs *regs, int scno)
{
	struct a52_r371_record r;
	u64 sequence;
	unsigned int slot;
	u32 event = A52_R371_EVT_ENTRY;

	if (!READ_ONCE(a52_r371_sideband) || !a52_r371_is_target())
		return 0;

	sequence = (u64)atomic64_inc_return(&a52_r371_sys_sequence);
	slot = A52_R371_SYSCALL_FIRST +
	       (unsigned int)((sequence - 1ULL) % A52_R371_SYSCALL_SLOTS);
	if (scno == __NR_exit || scno == __NR_exit_group)
		event = A52_R371_EVT_EXIT_ENTRY;

	a52_r371_fill_current(&r, regs, scno, event, sequence,
			      (s64)(1ULL << 63), slot);
	a52_r371_write_slot(slot, &r);
	return sequence;
}

static void a52_r371_worker_return(struct pt_regs *regs, int scno, u64 token)
{
	struct a52_r371_record r;
	unsigned int slot;

	if (!token || !READ_ONCE(a52_r371_sideband) || !a52_r371_is_target())
		return;

	slot = A52_R371_SYSCALL_FIRST +
	       (unsigned int)((token - 1ULL) % A52_R371_SYSCALL_SLOTS);
	a52_r371_fill_current(&r, regs, scno, A52_R371_EVT_RETURN, token,
			      (s64)regs->regs[0], slot);
	a52_r371_write_slot(slot, &r);
}

static void a52_r371_snapshot_target(void)
{
	struct a52_r371_record r;
	struct task_struct *p;
	u64 sequence;
	unsigned int slot;
	u32 pid = READ_ONCE(a52_r371_target_pid);

	if (!READ_ONCE(a52_r371_sideband) || !pid)
		return;

	sequence = (u64)atomic64_inc_return(&a52_r371_snap_sequence);
	slot = A52_R371_SNAPSHOT_FIRST +
	       (unsigned int)((sequence - 1ULL) % A52_R371_SNAPSHOT_SLOTS);

	memset(&r, 0, sizeof(r));
	r.magic = A52_R371_MAGIC;
	r.ns = ktime_get_ns();
	r.sequence = sequence;
	r.target_pid = pid;
	r.target_tgid = READ_ONCE(a52_r371_target_tgid);
	r.clone_flags = READ_ONCE(a52_r371_clone_flags);
	r.event = A52_R371_EVT_MISSING;
	r.syscallno = ~0U;
	r.cpu = (u32)raw_smp_processor_id();
	r.ring_index = slot;
	r.commit = A52_R371_COMMIT;
	r.version = A52_R371_VERSION;

	p = find_get_task_by_vpid((pid_t)pid);
	if (p) {
		r.event = A52_R371_EVT_SNAPSHOT;
		a52_r371_fill_task(&r, p);
		put_task_struct(p);
	}
	a52_r371_write_slot(slot, &r);
}

static int a52_r371_sampler_fn(void *unused)
{
	while (!kthread_should_stop()) {
		a52_r371_snapshot_target();
		if (msleep_interruptible(1000) && kthread_should_stop())
			break;
	}
	return 0;
}

static int __init __used a52_r371_init(void)
{
	BUILD_BUG_ON(sizeof(struct a52_r371_record) != A52_R371_SLOT_BYTES);
	BUILD_BUG_ON(A52_R371_SLOTS_PER_COPY * A52_R371_SLOT_BYTES !=
		     A52_R371_COPY_BYTES);
	BUILD_BUG_ON(A52_R371_HEADER_SLOTS + A52_R371_SYSCALL_SLOTS +
		     A52_R371_SNAPSHOT_SLOTS != A52_R371_SLOTS_PER_COPY);

	a52_r371_sideband = memremap(A52_R371_SIDEBAND_PHYS,
		A52_R371_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r371_sideband)
		return 0;

	memset(a52_r371_sideband, 0, A52_R371_SIDEBAND_BYTES);
	atomic64_set(&a52_r371_sys_sequence, 0);
	atomic64_set(&a52_r371_snap_sequence, 0);
	WRITE_ONCE(a52_r371_target_pid, 0);
	WRITE_ONCE(a52_r371_target_tgid, 0);
	WRITE_ONCE(a52_r371_clone_flags, 0);
	wmb();
	__flush_dcache_area(a52_r371_sideband, A52_R371_SIDEBAND_BYTES);

	a52_r371_sampler_task =
		kthread_run(a52_r371_sampler_fn, NULL, "a52_r371_sampler");
	if (IS_ERR(a52_r371_sampler_task))
		a52_r371_sampler_task = NULL;

	return 0;
}
/* Phase372 owns 0xB1BF0000..0xB1BF7FFF at runtime. */

static const char a52_r370_marker[] __used =
	"A52_PHASE370_LATEST_APEX_WORKER_V1";
static void *a52_r370_sideband;
static atomic64_t a52_r370_sequence = ATOMIC64_INIT(0);
static u32 a52_r370_target_pid;
static u32 a52_r370_target_tgid;

static bool a52_r370_apexd_group_leader(void)
{
	return current->pid == current->tgid &&
	       !strncmp(current->comm, "apexd", TASK_COMM_LEN);
}

static void a52_r370_copy_user_string(u8 *dst, size_t cap,
				      const char __user *src)
{
	long n;

	if (!dst || !cap || !src)
		return;
	n = strncpy_from_user((char *)dst, src, cap);
	if (n < 0)
		return;
	dst[cap - 1] = '\0';
}

static void a52_r370_copy_writev(struct a52_r370_record *r,
				 struct pt_regs *regs)
{
	struct iovec iov[4];
	unsigned int nr;
	unsigned int i;
	unsigned long iovcnt = regs->regs[2];
	size_t left = sizeof(r->payload);
	size_t n;
	u8 *dst = r->payload;

	if (!iovcnt)
		return;

	nr = min_t(unsigned int, (unsigned int)iovcnt, ARRAY_SIZE(iov));
	if (copy_from_user(iov, (const void __user *)regs->regs[1],
			   nr * sizeof(iov[0])))
		return;

	for (i = 0; i < nr && left; i++) {
		n = min_t(size_t, iov[i].iov_len, left);
		if (!n)
			continue;
		if (copy_from_user(dst, iov[i].iov_base, n))
			break;
		dst += n;
		left -= n;
	}
	r->payload_len = (u32)(sizeof(r->payload) - left);
	r->payload_kind = A52_R370_PAYLOAD_WRITEV;
}

static void a52_r370_capture_payload(struct a52_r370_record *r,
				      struct pt_regs *regs, int scno)
{
	const char __user *p0 =
		(const char __user *)(unsigned long)regs->orig_x0;
	const char __user *p1 =
		(const char __user *)(unsigned long)regs->regs[1];
	const char __user *p2 =
		(const char __user *)(unsigned long)regs->regs[2];

	if (scno == __NR_writev) {
		a52_r370_copy_writev(r, regs);
		return;
	}

	switch (scno) {
	case __NR_openat:
	case __NR_newfstatat:
	case __NR_faccessat:
	case __NR_mkdirat:
	case __NR_unlinkat:
	case __NR_readlinkat:
		r->payload_kind = A52_R370_PAYLOAD_PATH_X1;
		a52_r370_copy_user_string(r->payload, sizeof(r->payload), p1);
		break;
#ifdef __NR_openat2
	case __NR_openat2:
		r->payload_kind = A52_R370_PAYLOAD_PATH_X1;
		a52_r370_copy_user_string(r->payload, sizeof(r->payload), p1);
		break;
#endif
#ifdef __NR_statx
	case __NR_statx:
		r->payload_kind = A52_R370_PAYLOAD_PATH_X1;
		a52_r370_copy_user_string(r->payload, sizeof(r->payload), p1);
		break;
#endif
	case __NR_mount:
		r->payload_kind = A52_R370_PAYLOAD_MOUNT;
		a52_r370_copy_user_string(r->payload, 20, p0);
		a52_r370_copy_user_string(r->payload + 20, 28, p1);
		a52_r370_copy_user_string(r->payload + 48, 12, p2);
		break;
	default:
		break;
	}

	if (r->payload_kind && !r->payload_len)
		r->payload_len = sizeof(r->payload);
}

static void a52_r370_fill(struct a52_r370_record *r,
			  struct pt_regs *regs, int scno, u32 event,
			  u64 sequence, s64 ret, unsigned int slot)
{
	unsigned int i;

	memset(r, 0, sizeof(*r));
	r->magic = A52_R370_MAGIC;
	r->ns = ktime_get_ns();
	r->sequence = sequence;
	r->target_pid = READ_ONCE(a52_r370_target_pid);
	r->target_tgid = READ_ONCE(a52_r370_target_tgid);
	r->pc = regs->pc;
	r->lr = regs->regs[30];
	r->sp = regs->sp;
	r->mm = (u64)(unsigned long)current->mm;
	r->clear_child_tid =
		(u64)(unsigned long)READ_ONCE(current->clear_child_tid);
	r->x[0] = regs->orig_x0;
	for (i = 1; i < ARRAY_SIZE(r->x); i++)
		r->x[i] = regs->regs[i];
	r->ret = ret;
	r->event = event;
	r->syscallno = (u32)scno;
	r->cpu = (u32)raw_smp_processor_id();
	r->pid = (u32)current->pid;
	r->tgid = (u32)current->tgid;
	r->ppid = (u32)task_ppid_nr(current);
	r->ring_index = slot;
	r->commit = A52_R370_COMMIT;
	r->version = A52_R370_VERSION;
	memcpy(r->comm, current->comm, TASK_COMM_LEN);
}

static void a52_r370_write_slot(unsigned int slot,
				const struct a52_r370_record *r)
{
	unsigned int pos;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r370_sideband) || !r ||
	    slot >= A52_R370_SLOTS_PER_COPY)
		return;

	pos = slot * A52_R370_SLOT_BYTES;
	dst0 = (u8 *)a52_r370_sideband + pos;
	dst1 = (u8 *)a52_r370_sideband + A52_R370_COPY_BYTES + pos;
	memcpy(dst0, r, sizeof(*r));
	memcpy(dst1, r, sizeof(*r));
	wmb();
	__flush_dcache_area(dst0, sizeof(*r));
	__flush_dcache_area(dst1, sizeof(*r));
}

static void a52_r370_write_target_header(struct pt_regs *regs, int scno,
					 s64 child)
{
	struct a52_r370_record r;
	unsigned int i;

	a52_r370_fill(&r, regs, scno, A52_R370_EVT_TARGET, 0, child, 0);
	for (i = 0; i < A52_R370_HEADER_SLOTS; i++) {
		r.ring_index = i;
		a52_r370_write_slot(i, &r);
	}
}

static void a52_r370_note_clone_return(struct pt_regs *regs, int scno)
{
	s64 child = (s64)regs->regs[0];

	if (!READ_ONCE(a52_r370_sideband) || !a52_r370_apexd_group_leader())
		return;
	if (scno != __NR_clone
#ifdef __NR_clone3
	    && scno != __NR_clone3
#endif
	    )
		return;
	if (child <= 0 || child > INT_MAX)
		return;

	WRITE_ONCE(a52_r370_target_tgid, (u32)current->tgid);
	WRITE_ONCE(a52_r370_target_pid, (u32)child);
	atomic64_set(&a52_r370_sequence, 0);
	a52_r370_write_target_header(regs, scno, child);
}

static bool a52_r370_is_target(void)
{
	u32 pid = READ_ONCE(a52_r370_target_pid);
	u32 tgid = READ_ONCE(a52_r370_target_tgid);

	return pid && current->pid == pid && current->tgid == tgid;
}

static u64 a52_r370_worker_enter(struct pt_regs *regs, int scno)
{
	struct a52_r370_record r;
	u64 sequence;
	unsigned int slot;
	u32 event = A52_R370_EVT_ENTRY;

	if (!READ_ONCE(a52_r370_sideband) || !a52_r370_is_target())
		return 0;

	sequence = (u64)atomic64_inc_return(&a52_r370_sequence);
	slot = A52_R370_RING_FIRST +
	       (unsigned int)((sequence - 1ULL) % A52_R370_RING_SLOTS);

	if (scno == __NR_exit || scno == __NR_exit_group)
		event = A52_R370_EVT_EXIT_ENTRY;

	a52_r370_fill(&r, regs, scno, event, sequence,
		      (s64)(1ULL << 63), slot);
	a52_r370_capture_payload(&r, regs, scno);
	a52_r370_write_slot(slot, &r);
	return sequence;
}

static void a52_r370_worker_return(struct pt_regs *regs, int scno, u64 token)
{
	struct a52_r370_record r;
	unsigned int slot;

	if (!token || !READ_ONCE(a52_r370_sideband) || !a52_r370_is_target())
		return;

	slot = A52_R370_RING_FIRST +
	       (unsigned int)((token - 1ULL) % A52_R370_RING_SLOTS);
	a52_r370_fill(&r, regs, scno, A52_R370_EVT_RETURN, token,
		      (s64)regs->regs[0], slot);
	a52_r370_capture_payload(&r, regs, scno);
	a52_r370_write_slot(slot, &r);
}

static int __init __used a52_r370_init(void)
{
	BUILD_BUG_ON(sizeof(struct a52_r370_record) != A52_R370_SLOT_BYTES);
	BUILD_BUG_ON(A52_R370_SLOTS_PER_COPY * A52_R370_SLOT_BYTES !=
		     A52_R370_COPY_BYTES);
	BUILD_BUG_ON(A52_R370_HEADER_SLOTS + A52_R370_RING_SLOTS !=
		     A52_R370_SLOTS_PER_COPY);

	a52_r370_sideband = memremap(A52_R370_SIDEBAND_PHYS,
		A52_R370_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r370_sideband)
		return 0;

	memset(a52_r370_sideband, 0, A52_R370_SIDEBAND_BYTES);
	atomic64_set(&a52_r370_sequence, 0);
	WRITE_ONCE(a52_r370_target_pid, 0);
	WRITE_ONCE(a52_r370_target_tgid, 0);
	wmb();
	__flush_dcache_area(a52_r370_sideband, A52_R370_SIDEBAND_BYTES);
	return 0;
}
/* Phase371 owns 0xB1BF0000..0xB1BF7FFF at runtime. */

static const char a52_r369_marker[] __used =
	"A52_PHASE369_POST_APEX_FRONTIER_V1";
static void *a52_r369_sideband;
static atomic64_t a52_r369_main_sequence = ATOMIC64_INIT(0);
static atomic64_t a52_r369_exec_sequence = ATOMIC64_INIT(0);

static bool a52_r369_apexd_group_leader(void)
{
	return current->pid == current->tgid &&
	       !strncmp(current->comm, "apexd", TASK_COMM_LEN);
}

static void a52_r369_copy_user_string(u8 *dst, size_t cap,
				      const char __user *src)
{
	long n;

	if (!dst || !cap || !src)
		return;
	n = strncpy_from_user((char *)dst, src, cap);
	if (n < 0)
		return;
	dst[cap - 1] = '\0';
}

static void a52_r369_copy_writev(struct a52_r369_record *r,
				 struct pt_regs *regs)
{
	struct iovec iov[4];
	unsigned int nr;
	unsigned int i;
	unsigned long iovcnt = regs->regs[2];
	size_t left = sizeof(r->payload);
	size_t n;
	u8 *dst = r->payload;

	if (!iovcnt)
		return;

	nr = min_t(unsigned int, (unsigned int)iovcnt, ARRAY_SIZE(iov));
	if (copy_from_user(iov, (const void __user *)regs->regs[1],
			   nr * sizeof(iov[0])))
		return;

	for (i = 0; i < nr && left; i++) {
		n = min_t(size_t, iov[i].iov_len, left);
		if (!n)
			continue;
		if (copy_from_user(dst, iov[i].iov_base, n))
			break;
		dst += n;
		left -= n;
	}
	r->payload_len = (u32)(sizeof(r->payload) - left);
	r->payload_kind = A52_R369_PAYLOAD_WRITEV;
}

static void a52_r369_capture_sleep(struct a52_r369_record *r,
				   struct pt_regs *regs, int scno)
{
	struct {
		s64 tv_sec;
		s64 tv_nsec;
	} ts;
	const void __user *p = NULL;

	if (scno == __NR_nanosleep)
		p = (const void __user *)(unsigned long)regs->orig_x0;
#ifdef __NR_clock_nanosleep
	else if (scno == __NR_clock_nanosleep)
		p = (const void __user *)(unsigned long)regs->regs[2];
#endif
	if (!p)
		return;
	if (copy_from_user(&ts, p, sizeof(ts)))
		return;
	r->sleep_sec = ts.tv_sec;
	r->sleep_nsec = ts.tv_nsec;
}

static void a52_r369_capture_main_payload(struct a52_r369_record *r,
					  struct pt_regs *regs, int scno)
{
	const char __user *path = NULL;

	a52_r369_capture_sleep(r, regs, scno);

	if (scno == __NR_writev) {
		a52_r369_copy_writev(r, regs);
		return;
	}

	switch (scno) {
	case __NR_openat:
	case __NR_newfstatat:
	case __NR_faccessat:
	case __NR_mkdirat:
	case __NR_unlinkat:
	case __NR_readlinkat:
		path = (const char __user *)(unsigned long)regs->regs[1];
		break;
#ifdef __NR_openat2
	case __NR_openat2:
		path = (const char __user *)(unsigned long)regs->regs[1];
		break;
#endif
#ifdef __NR_faccessat2
	case __NR_faccessat2:
		path = (const char __user *)(unsigned long)regs->regs[1];
		break;
#endif
#ifdef __NR_statx
	case __NR_statx:
		path = (const char __user *)(unsigned long)regs->regs[1];
		break;
#endif
	default:
		break;
	}

	if (path) {
		r->payload_kind = A52_R369_PAYLOAD_PATH;
		a52_r369_copy_user_string(r->payload, sizeof(r->payload), path);
		r->payload_len = strnlen((const char *)r->payload,
					sizeof(r->payload));
	}
}

static void a52_r369_fill(struct a52_r369_record *r,
			  struct pt_regs *regs, int scno, u32 event,
			  u64 sequence, s64 ret, unsigned int slot)
{
	unsigned int i;

	memset(r, 0, sizeof(*r));
	r->magic = A52_R369_MAGIC;
	r->ns = ktime_get_ns();
	r->sequence = sequence;
	r->pc = regs->pc;
	r->lr = regs->regs[30];
	r->sp = regs->sp;
	r->mm = (u64)(unsigned long)current->mm;
	r->x[0] = regs->orig_x0;
	for (i = 1; i < ARRAY_SIZE(r->x); i++)
		r->x[i] = regs->regs[i];
	r->ret = ret;
	r->event = event;
	r->syscallno = (u32)scno;
	r->cpu = (u32)raw_smp_processor_id();
	r->pid = (u32)current->pid;
	r->tgid = (u32)current->tgid;
	r->ppid = (u32)task_ppid_nr(current);
	r->ring_index = slot;
	r->commit = A52_R369_COMMIT;
	r->version = A52_R369_VERSION;
	memcpy(r->comm, current->comm, TASK_COMM_LEN);
}

static void a52_r369_write_slot(unsigned int slot,
				const struct a52_r369_record *r)
{
	unsigned int pos;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r369_sideband) || !r ||
	    slot >= A52_R369_SLOTS_PER_COPY)
		return;

	pos = slot * A52_R369_SLOT_BYTES;
	dst0 = (u8 *)a52_r369_sideband + pos;
	dst1 = (u8 *)a52_r369_sideband + A52_R369_COPY_BYTES + pos;
	memcpy(dst0, r, sizeof(*r));
	memcpy(dst1, r, sizeof(*r));
	wmb();
	__flush_dcache_area(dst0, sizeof(*r));
	__flush_dcache_area(dst1, sizeof(*r));
}

static u64 a52_r369_main_enter(struct pt_regs *regs, int scno)
{
	struct a52_r369_record r;
	u64 sequence;
	unsigned int slot;

	if (!READ_ONCE(a52_r369_sideband) || !a52_r369_apexd_group_leader())
		return 0;

	sequence = (u64)atomic64_inc_return(&a52_r369_main_sequence);
	slot = A52_R369_MAIN_FIRST +
	       (unsigned int)((sequence - 1ULL) % A52_R369_MAIN_SLOTS);
	a52_r369_fill(&r, regs, scno, A52_R369_EVT_MAIN_ENTRY, sequence,
		      (s64)(1ULL << 63), slot);
	a52_r369_capture_main_payload(&r, regs, scno);
	a52_r369_write_slot(slot, &r);
	return sequence;
}

static void a52_r369_main_return(struct pt_regs *regs, int scno, u64 token)
{
	struct a52_r369_record r;
	u64 sequence;
	unsigned int slot;

	if (!token || !READ_ONCE(a52_r369_sideband) ||
	    !a52_r369_apexd_group_leader())
		return;

	sequence = (u64)atomic64_inc_return(&a52_r369_main_sequence);
	slot = A52_R369_MAIN_FIRST +
	       (unsigned int)((sequence - 1ULL) % A52_R369_MAIN_SLOTS);
	a52_r369_fill(&r, regs, scno, A52_R369_EVT_MAIN_RETURN, sequence,
		      (s64)regs->regs[0], slot);
	a52_r369_capture_main_payload(&r, regs, scno);
	a52_r369_write_slot(slot, &r);
}

static void a52_r369_maybe_init_exec(struct pt_regs *regs, int scno)
{
	struct a52_r369_record r;
	const char __user *path;
	u64 sequence;
	unsigned int slot;

	if (!READ_ONCE(a52_r369_sideband) || task_ppid_nr(current) != 1)
		return;
	if (scno != __NR_execve && scno != __NR_execveat)
		return;

	if (scno == __NR_execve)
		path = (const char __user *)(unsigned long)regs->orig_x0;
	else
		path = (const char __user *)(unsigned long)regs->regs[1];

	sequence = (u64)atomic64_inc_return(&a52_r369_exec_sequence);
	slot = A52_R369_EXEC_FIRST +
	       (unsigned int)((sequence - 1ULL) % A52_R369_EXEC_SLOTS);
	a52_r369_fill(&r, regs, scno, A52_R369_EVT_INIT_EXEC, sequence,
		      (s64)(1ULL << 63), slot);
	r.payload_kind = A52_R369_PAYLOAD_PATH;
	a52_r369_copy_user_string(r.payload, sizeof(r.payload), path);
	r.payload_len = strnlen((const char *)r.payload, sizeof(r.payload));
	a52_r369_write_slot(slot, &r);
}

static int __init __used a52_r369_init(void)
{
	BUILD_BUG_ON(sizeof(struct a52_r369_record) != A52_R369_SLOT_BYTES);
	BUILD_BUG_ON(A52_R369_SLOTS_PER_COPY * A52_R369_SLOT_BYTES !=
		     A52_R369_COPY_BYTES);
	BUILD_BUG_ON(A52_R369_MAIN_SLOTS + A52_R369_EXEC_SLOTS !=
		     A52_R369_SLOTS_PER_COPY);

	a52_r369_sideband = memremap(A52_R369_SIDEBAND_PHYS,
		A52_R369_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r369_sideband)
		return 0;

	memset(a52_r369_sideband, 0, A52_R369_SIDEBAND_BYTES);
	atomic64_set(&a52_r369_main_sequence, 0);
	atomic64_set(&a52_r369_exec_sequence, 0);
	wmb();
	__flush_dcache_area(a52_r369_sideband, A52_R369_SIDEBAND_BYTES);
	return 0;
}
/* Phase370 owns 0xB1BF0000..0xB1BF7FFF at runtime. */

static const char a52_r368_marker[] __used =
	"A52_PHASE368_APEXD_WORKER_SYSCALL_MAP_V1";
static void *a52_r368_sideband;
static atomic64_t a52_r368_sequence = ATOMIC64_INIT(0);
static atomic_t a52_r368_owner[A52_R368_SLOTS];

static bool a52_r368_apexd_thread(void)
{
	char comm[TASK_COMM_LEN];

	if (!current->group_leader)
		return false;
	get_task_comm(comm, current->group_leader);
	return !strncmp(comm, "apexd", TASK_COMM_LEN);
}

static int a52_r368_slot_for_pid(u32 pid)
{
	unsigned int start = pid & (A52_R368_SLOTS - 1U);
	unsigned int i;

	for (i = 0; i < A52_R368_SLOTS; i++) {
		unsigned int slot = (start + i) & (A52_R368_SLOTS - 1U);
		int owner = atomic_read(&a52_r368_owner[slot]);

		if (owner == (int)pid)
			return (int)slot;
		if (!owner &&
		    atomic_cmpxchg(&a52_r368_owner[slot], 0, (int)pid) == 0)
			return (int)slot;
	}
	return -1;
}

static void a52_r368_copy_user_string(u8 *dst, size_t cap,
				      const char __user *src)
{
	long n;

	if (!dst || !cap || !src)
		return;
	n = strncpy_from_user((char *)dst, src, cap);
	if (n < 0)
		return;
	dst[cap - 1] = '\0';
}

static void a52_r368_capture_payload(struct a52_r368_record *r, int scno)
{
	const char __user *p0 = (const char __user *)(unsigned long)r->x[0];
	const char __user *p1 = (const char __user *)(unsigned long)r->x[1];
	const char __user *p2 = (const char __user *)(unsigned long)r->x[2];
	const char __user *p3 = (const char __user *)(unsigned long)r->x[3];

	switch (scno) {
	case __NR_openat:
	case __NR_newfstatat:
	case __NR_faccessat:
	case __NR_mkdirat:
	case __NR_unlinkat:
	case __NR_readlinkat:
		r->payload_kind = A52_R368_PAYLOAD_PATH_X1;
		a52_r368_copy_user_string(r->payload, sizeof(r->payload), p1);
		break;
#ifdef __NR_openat2
	case __NR_openat2:
		r->payload_kind = A52_R368_PAYLOAD_PATH_X1;
		a52_r368_copy_user_string(r->payload, sizeof(r->payload), p1);
		break;
#endif
#ifdef __NR_faccessat2
	case __NR_faccessat2:
		r->payload_kind = A52_R368_PAYLOAD_PATH_X1;
		a52_r368_copy_user_string(r->payload, sizeof(r->payload), p1);
		break;
#endif
#ifdef __NR_statx
	case __NR_statx:
		r->payload_kind = A52_R368_PAYLOAD_PATH_X1;
		a52_r368_copy_user_string(r->payload, sizeof(r->payload), p1);
		break;
#endif
	case __NR_mount:
		r->payload_kind = A52_R368_PAYLOAD_MOUNT;
		a52_r368_copy_user_string(r->payload, 24, p0);
		a52_r368_copy_user_string(r->payload + 24, 44, p1);
		a52_r368_copy_user_string(r->payload + 68, 12, p2);
		break;
#ifdef __NR_renameat
	case __NR_renameat:
		r->payload_kind = A52_R368_PAYLOAD_RENAME;
		a52_r368_copy_user_string(r->payload, 40, p1);
		a52_r368_copy_user_string(r->payload + 40, 40, p3);
		break;
#endif
#ifdef __NR_renameat2
	case __NR_renameat2:
		r->payload_kind = A52_R368_PAYLOAD_RENAME;
		a52_r368_copy_user_string(r->payload, 40, p1);
		a52_r368_copy_user_string(r->payload + 40, 40, p3);
		break;
#endif
	default:
		break;
	}

	if (r->payload_kind)
		r->payload_len = sizeof(r->payload);
}

static void a52_r368_fill(struct a52_r368_record *r,
			  struct pt_regs *regs, int scno, u32 event,
			  u64 sequence, s64 ret, unsigned int slot)
{
	unsigned int i;

	memset(r, 0, sizeof(*r));
	r->magic = A52_R368_MAGIC;
	r->ns = ktime_get_ns();
	r->sequence = sequence;
	r->pc = regs->pc;
	r->lr = regs->regs[30];
	r->sp = regs->sp;
	r->clear_child_tid =
		(u64)(unsigned long)READ_ONCE(current->clear_child_tid);
	r->mm = (u64)(unsigned long)current->mm;
	r->x[0] = regs->orig_x0;
	for (i = 1; i < ARRAY_SIZE(r->x); i++)
		r->x[i] = regs->regs[i];
	r->ret = ret;
	r->event = event;
	r->syscallno = (u32)scno;
	r->cpu = (u32)raw_smp_processor_id();
	r->pid = (u32)current->pid;
	r->tgid = (u32)current->tgid;
	r->slot_index = slot;
	r->commit = A52_R368_COMMIT;
	r->version = A52_R368_VERSION;
	memcpy(r->comm, current->comm, TASK_COMM_LEN);
	a52_r368_capture_payload(r, scno);
}

static void a52_r368_write_slot(unsigned int slot,
				const struct a52_r368_record *r)
{
	unsigned int pos;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r368_sideband) || !r || slot >= A52_R368_SLOTS)
		return;

	pos = slot * A52_R368_SLOT_BYTES;
	dst0 = (u8 *)a52_r368_sideband + pos;
	dst1 = (u8 *)a52_r368_sideband + A52_R368_COPY_BYTES + pos;
	memcpy(dst0, r, sizeof(*r));
	memcpy(dst1, r, sizeof(*r));
	wmb();
	__flush_dcache_area(dst0, sizeof(*r));
	__flush_dcache_area(dst1, sizeof(*r));
}

static u64 a52_r368_sys_enter(struct pt_regs *regs, int scno)
{
	struct a52_r368_record r;
	u64 sequence;
	u32 event = A52_R368_EVT_ENTRY;
	int slot;

	if (!READ_ONCE(a52_r368_sideband) || !a52_r368_apexd_thread())
		return 0;

	slot = a52_r368_slot_for_pid((u32)current->pid);
	if (slot < 0)
		return 0;

	sequence = (u64)atomic64_inc_return(&a52_r368_sequence);
	if (scno == __NR_exit || scno == __NR_exit_group)
		event = A52_R368_EVT_EXIT_ENTRY;

	a52_r368_fill(&r, regs, scno, event, sequence, (s64)(1ULL << 63),
		      (unsigned int)slot);
	a52_r368_write_slot((unsigned int)slot, &r);
	return sequence;
}

static void a52_r368_sys_return(struct pt_regs *regs, int scno, u64 sequence)
{
	struct a52_r368_record r;
	int slot;

	if (!sequence || !READ_ONCE(a52_r368_sideband) ||
	    !a52_r368_apexd_thread())
		return;

	slot = a52_r368_slot_for_pid((u32)current->pid);
	if (slot < 0)
		return;

	a52_r368_fill(&r, regs, scno, A52_R368_EVT_RETURN, sequence,
		      (s64)regs->regs[0], (unsigned int)slot);
	a52_r368_write_slot((unsigned int)slot, &r);
}

static int __init __used a52_r368_init(void)
{
	unsigned int i;

	BUILD_BUG_ON(sizeof(struct a52_r368_record) != A52_R368_SLOT_BYTES);
	BUILD_BUG_ON(A52_R368_SLOTS * A52_R368_SLOT_BYTES !=
		     A52_R368_COPY_BYTES);
	BUILD_BUG_ON((A52_R368_SLOTS & (A52_R368_SLOTS - 1U)) != 0);

	a52_r368_sideband = memremap(A52_R368_SIDEBAND_PHYS,
		A52_R368_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r368_sideband)
		return 0;

	memset(a52_r368_sideband, 0, A52_R368_SIDEBAND_BYTES);
	atomic64_set(&a52_r368_sequence, 0);
	for (i = 0; i < A52_R368_SLOTS; i++)
		atomic_set(&a52_r368_owner[i], 0);
	wmb();
	__flush_dcache_area(a52_r368_sideband, A52_R368_SIDEBAND_BYTES);
	return 0;
}
/* Phase369 owns 0xB1BF0000..0xB1BF7FFF at runtime. */

static const char a52_r366_marker[] __used =
	"A52_PHASE366_APEXD_FUTEX_THREADS_V1";
static void *a52_r366_sideband;
static atomic64_t a52_r366_sequence = ATOMIC64_INIT(0);
static u64 a52_r366_main_target;

static bool a52_r366_apexd_thread(void)
{
	char comm[TASK_COMM_LEN];

	if (!current->group_leader)
		return false;
	get_task_comm(comm, current->group_leader);
	return !strncmp(comm, "apexd", TASK_COMM_LEN);
}

static bool a52_r366_wait_op(u32 op)
{
	u32 cmd = op & FUTEX_CMD_MASK;

	return cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET;
}

static void a52_r366_fill(struct a52_r366_record *r,
			  struct pt_regs *regs, u32 event,
			  u64 sequence, s64 ret)
{
	memset(r, 0, sizeof(*r));
	r->magic = A52_R366_MAGIC;
	r->ns = ktime_get_ns();
	r->sequence = sequence;
	r->uaddr = regs->orig_x0;
	r->op = (u32)regs->regs[1];
	r->val = regs->regs[2];
	r->timeout = regs->regs[3];
	r->uaddr2 = regs->regs[4];
	r->val3 = regs->regs[5];
	r->target_uaddr = READ_ONCE(a52_r366_main_target);
	r->ret = ret;
	r->event = event;
	r->syscallno = __NR_futex;
	r->cpu = (u32)raw_smp_processor_id();
	r->pid = (u32)current->pid;
	r->tgid = (u32)current->tgid;
	r->ring_index = (u32)((sequence - 1ULL) % A52_R366_RING_SLOTS);
	r->commit = A52_R366_COMMIT;
	r->version = A52_R366_VERSION;
	memcpy(r->comm, current->comm, TASK_COMM_LEN);
}

static void a52_r366_write_slot(unsigned int slot,
				const struct a52_r366_record *r)
{
	unsigned int pos;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r366_sideband) || !r ||
	    slot >= A52_R366_SLOTS_PER_COPY)
		return;

	pos = slot * A52_R366_SLOT_BYTES;
	dst0 = (u8 *)a52_r366_sideband + pos;
	dst1 = (u8 *)a52_r366_sideband + A52_R366_COPY_BYTES + pos;
	memcpy(dst0, r, sizeof(*r));
	memcpy(dst1, r, sizeof(*r));
	wmb();
	__flush_dcache_area(dst0, sizeof(*r));
	__flush_dcache_area(dst1, sizeof(*r));
}

static void a52_r366_write_latch(const struct a52_r366_record *r)
{
	unsigned int i;

	for (i = 0; i < A52_R366_LATCH_REPLICAS; i++)
		a52_r366_write_slot(A52_R366_LATCH_FIRST + i, r);
}

static u64 a52_r366_sys_enter(struct pt_regs *regs, int scno)
{
	struct a52_r366_record r;
	u64 sequence;
	unsigned int slot;
	u32 op;

	if (scno != __NR_futex || !READ_ONCE(a52_r366_sideband) ||
	    !a52_r366_apexd_thread())
		return 0;

	op = (u32)regs->regs[1];
	if (current->pid == current->tgid && a52_r366_wait_op(op))
		WRITE_ONCE(a52_r366_main_target, regs->orig_x0);

	sequence = (u64)atomic64_inc_return(&a52_r366_sequence);
	slot = (unsigned int)((sequence - 1ULL) % A52_R366_RING_SLOTS);
	a52_r366_fill(&r, regs, A52_R366_EVT_ENTRY, sequence,
		      (s64)(1ULL << 63));
	a52_r366_write_slot(slot, &r);

	if (current->pid == current->tgid && a52_r366_wait_op(op))
		a52_r366_write_latch(&r);

	return sequence;
}

static void a52_r366_sys_return(struct pt_regs *regs, int scno, u64 sequence)
{
	struct a52_r366_record r;
	unsigned int slot;
	u32 op;

	if (!sequence || scno != __NR_futex ||
	    !READ_ONCE(a52_r366_sideband) || !a52_r366_apexd_thread())
		return;

	op = (u32)regs->regs[1];
	slot = (unsigned int)((sequence - 1ULL) % A52_R366_RING_SLOTS);
	a52_r366_fill(&r, regs, A52_R366_EVT_RETURN, sequence,
		      (s64)regs->regs[0]);
	a52_r366_write_slot(slot, &r);

	if (current->pid == current->tgid && a52_r366_wait_op(op))
		a52_r366_write_latch(&r);
}

static int __init __used a52_r366_init(void)
{
	BUILD_BUG_ON(sizeof(struct a52_r366_record) != A52_R366_SLOT_BYTES);
	BUILD_BUG_ON(A52_R366_SLOTS_PER_COPY * A52_R366_SLOT_BYTES !=
		     A52_R366_COPY_BYTES);
	BUILD_BUG_ON(A52_R366_RING_SLOTS + A52_R366_LATCH_REPLICAS !=
		     A52_R366_SLOTS_PER_COPY);

	a52_r366_sideband = memremap(A52_R366_SIDEBAND_PHYS,
		A52_R366_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r366_sideband)
		return 0;

	memset(a52_r366_sideband, 0, A52_R366_SIDEBAND_BYTES);
	atomic64_set(&a52_r366_sequence, 0);
	WRITE_ONCE(a52_r366_main_target, 0);
	wmb();
	__flush_dcache_area(a52_r366_sideband, A52_R366_SIDEBAND_BYTES);
	return 0;
}
/* Phase367 owns 0xB1BF0000..0xB1BF7FFF at runtime. */

static const char a52_r365_marker[] __used =
	"A52_PHASE365_APEXD_WRITEV_FUTEX_V1";
static void *a52_r365_sideband;
static atomic64_t a52_r365_sequence = ATOMIC64_INIT(0);

static bool a52_r365_target(void)
{
	return current->pid == current->tgid &&
	       !strncmp(current->comm, "apexd", TASK_COMM_LEN);
}

static void a52_r365_copy_writev(struct a52_r365_record *r,
				 struct pt_regs *regs)
{
	struct iovec iov[8];
	unsigned long iovcnt = regs->regs[2];
	unsigned int nr;
	unsigned int i;
	size_t left = sizeof(r->payload);
	size_t n;
	u8 *dst = r->payload;

	r->fd = (s32)regs->orig_x0;
	r->iovcnt = (u32)iovcnt;
	if (!iovcnt)
		return;

	nr = min_t(unsigned int, (unsigned int)iovcnt, ARRAY_SIZE(iov));
	if (copy_from_user(iov, (const void __user *)regs->regs[1],
			   nr * sizeof(iov[0])))
		return;

	for (i = 0; i < nr && left; i++) {
		n = min_t(size_t, iov[i].iov_len, left);
		if (!n)
			continue;
		if (copy_from_user(dst, iov[i].iov_base, n))
			break;
		dst += n;
		left -= n;
	}
	r->payload_len = (u32)(sizeof(r->payload) - left);
}

static void a52_r365_fill(struct a52_r365_record *r,
			  struct pt_regs *regs, int scno, u32 event,
			  u64 sequence, s64 ret)
{
	unsigned int i;

	memset(r, 0, sizeof(*r));
	r->magic = A52_R365_MAGIC;
	r->ns = ktime_get_ns();
	r->sequence = sequence;
	r->pc = regs->pc;
	r->lr = regs->regs[30];
	r->sp = regs->sp;
	r->x[0] = regs->orig_x0;
	for (i = 1; i < ARRAY_SIZE(r->x); i++)
		r->x[i] = regs->regs[i];
	r->ret = ret;
	r->event = event;
	r->syscallno = (u32)scno;
	r->cpu = (u32)raw_smp_processor_id();
	r->pid = (u32)current->pid;
	r->tgid = (u32)current->tgid;
	r->ring_index = (u32)((sequence - 1ULL) % A52_R365_SLOTS);
	r->commit = A52_R365_COMMIT;
	r->version = A52_R365_VERSION;
	memcpy(r->comm, current->comm, TASK_COMM_LEN);

	if (scno == __NR_writev)
		a52_r365_copy_writev(r, regs);
}

static void a52_r365_write(unsigned int slot,
			   const struct a52_r365_record *r)
{
	unsigned int pos;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r365_sideband) || !r || slot >= A52_R365_SLOTS)
		return;

	pos = slot * A52_R365_SLOT_BYTES;
	dst0 = (u8 *)a52_r365_sideband + pos;
	dst1 = (u8 *)a52_r365_sideband + A52_R365_COPY_BYTES + pos;
	memcpy(dst0, r, sizeof(*r));
	memcpy(dst1, r, sizeof(*r));
	wmb();
	__flush_dcache_area(dst0, sizeof(*r));
	__flush_dcache_area(dst1, sizeof(*r));
}

static u64 a52_r365_sys_enter(struct pt_regs *regs, int scno)
{
	struct a52_r365_record r;
	u64 sequence;
	unsigned int slot;
	u32 event;

	if (!READ_ONCE(a52_r365_sideband) || !a52_r365_target())
		return 0;
	if (scno != __NR_writev && scno != __NR_futex)
		return 0;

	sequence = (u64)atomic64_inc_return(&a52_r365_sequence);
	slot = (unsigned int)((sequence - 1ULL) % A52_R365_SLOTS);
	event = scno == __NR_writev ?
		A52_R365_EVT_WRITEV_ENTRY : A52_R365_EVT_FUTEX_ENTRY;
	a52_r365_fill(&r, regs, scno, event, sequence,
		      (s64)(1ULL << 63));
	a52_r365_write(slot, &r);
	return sequence;
}

static void a52_r365_sys_return(struct pt_regs *regs, int scno, u64 sequence)
{
	struct a52_r365_record r;
	unsigned int slot;
	u32 event;

	if (!sequence || !READ_ONCE(a52_r365_sideband) || !a52_r365_target())
		return;

	slot = (unsigned int)((sequence - 1ULL) % A52_R365_SLOTS);
	event = scno == __NR_writev ?
		A52_R365_EVT_WRITEV_RETURN : A52_R365_EVT_FUTEX_RETURN;
	a52_r365_fill(&r, regs, scno, event, sequence, (s64)regs->regs[0]);
	a52_r365_write(slot, &r);
}

static int __init __used a52_r365_init(void)
{
	BUILD_BUG_ON(sizeof(struct a52_r365_record) != A52_R365_SLOT_BYTES);
	BUILD_BUG_ON(A52_R365_SLOTS * A52_R365_SLOT_BYTES !=
		     A52_R365_COPY_BYTES);

	a52_r365_sideband = memremap(A52_R365_SIDEBAND_PHYS,
		A52_R365_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r365_sideband)
		return 0;

	memset(a52_r365_sideband, 0, A52_R365_SIDEBAND_BYTES);
	atomic64_set(&a52_r365_sequence, 0);
	wmb();
	__flush_dcache_area(a52_r365_sideband, A52_R365_SIDEBAND_BYTES);
	return 0;
}
/* Phase366 owns 0xB1BF0000..0xB1BF7FFF at runtime. */

static const char a52_r364_marker[] __used =
	"A52_PHASE364_APEXD_SYSCALL_RING_V1";
static void *a52_r364_sideband;
static u64 a52_r364_sequence;

static bool a52_r364_target(void)
{
	return current->pid == current->tgid &&
	       !strncmp(current->comm, "apexd", TASK_COMM_LEN);
}

static void a52_r364_fill(struct a52_r364_record *r,
			  struct pt_regs *regs, int scno, u32 event,
			  u64 sequence, s64 ret)
{
	memset(r, 0, sizeof(*r));
	r->magic = A52_R364_MAGIC;
	r->ns = ktime_get_ns();
	r->sequence = sequence;
	r->pc = regs->pc;
	r->sp = regs->sp;
	r->pstate = regs->pstate;
	r->lr = regs->regs[30];
	r->x[0] = regs->orig_x0;
	r->x[1] = regs->regs[1];
	r->x[2] = regs->regs[2];
	r->x[3] = regs->regs[3];
	r->x[4] = regs->regs[4];
	r->x[5] = regs->regs[5];
	r->ret = ret;
	r->event = event;
	r->syscallno = (u32)scno;
	r->cpu = (u32)raw_smp_processor_id();
	r->pid = (u32)current->pid;
	r->tgid = (u32)current->tgid;
	r->ring_index = (u32)((sequence - 1ULL) % A52_R364_SLOTS);
	r->commit = A52_R364_COMMIT;
	r->version = A52_R364_VERSION;
	memcpy(r->comm, current->comm, TASK_COMM_LEN);
}

static void a52_r364_write(unsigned int slot,
			   const struct a52_r364_record *r)
{
	void *dst0;
	void *dst1;
	unsigned int pos;

	if (!READ_ONCE(a52_r364_sideband) || !r || slot >= A52_R364_SLOTS)
		return;

	pos = slot * A52_R364_SLOT_BYTES;
	dst0 = (u8 *)a52_r364_sideband + pos;
	dst1 = (u8 *)a52_r364_sideband + A52_R364_COPY_BYTES + pos;
	memcpy(dst0, r, sizeof(*r));
	memcpy(dst1, r, sizeof(*r));
	wmb();
	__flush_dcache_area(dst0, sizeof(*r));
	__flush_dcache_area(dst1, sizeof(*r));
}

static u64 a52_r364_sys_enter(struct pt_regs *regs, int scno)
{
	struct a52_r364_record r;
	unsigned int slot;
	u64 sequence;

	if (!READ_ONCE(a52_r364_sideband) || !a52_r364_target())
		return 0;

	sequence = ++a52_r364_sequence;
	slot = (unsigned int)((sequence - 1ULL) % A52_R364_SLOTS);
	a52_r364_fill(&r, regs, scno, A52_R364_EVT_ENTRY, sequence,
		      (s64)(1ULL << 63));
	a52_r364_write(slot, &r);
	return sequence;
}

static void a52_r364_sys_return(struct pt_regs *regs, int scno, u64 sequence)
{
	struct a52_r364_record r;
	unsigned int slot;

	if (!sequence || !READ_ONCE(a52_r364_sideband) || !a52_r364_target())
		return;

	slot = (unsigned int)((sequence - 1ULL) % A52_R364_SLOTS);
	a52_r364_fill(&r, regs, scno, A52_R364_EVT_RETURN, sequence,
		      (s64)regs->regs[0]);
	a52_r364_write(slot, &r);
}

static int __init __used a52_r364_init(void)
{
	BUILD_BUG_ON(sizeof(struct a52_r364_record) != A52_R364_SLOT_BYTES);
	BUILD_BUG_ON(A52_R364_SLOTS * A52_R364_SLOT_BYTES !=
		     A52_R364_COPY_BYTES);

	a52_r364_sideband = memremap(A52_R364_SIDEBAND_PHYS,
		A52_R364_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r364_sideband)
		return 0;

	memset(a52_r364_sideband, 0, A52_R364_SIDEBAND_BYTES);
	a52_r364_sequence = 0;
	wmb();
	__flush_dcache_area(a52_r364_sideband, A52_R364_SIDEBAND_BYTES);
	return 0;
}
/* Phase365 owns 0xB1BF0000..0xB1BF7FFF at runtime. */

static const char a52_r359_marker[] __used =
	"A52_PHASE359_PID1_WRITEV_PAYLOAD_V1";
static void *a52_r359_sideband;
static struct a52_r359_record a52_r359_ring[A52_R359_RING_SLOTS];
static u64 a52_r359_sequence;
static u32 a52_r359_count;

static void a52_r359_copy_payload(struct a52_r359_record *r,
				  struct pt_regs *regs)
{
	struct iovec iov[8];
	unsigned long iovcnt = regs->regs[2];
	unsigned int nr;
	unsigned int i;
	size_t left = sizeof(r->payload);
	size_t n;
	u8 *dst = r->payload;

	if (!iovcnt)
		return;
	nr = iovcnt > ARRAY_SIZE(iov) ? ARRAY_SIZE(iov) : (unsigned int)iovcnt;
	if (copy_from_user(iov, (const void __user *)regs->regs[1],
			   nr * sizeof(iov[0])))
		return;

	r->iovcnt = (u32)iovcnt;
	for (i = 0; i < nr && left; i++) {
		n = iov[i].iov_len;
		if (n > left)
			n = left;
		if (!n)
			continue;
		if (copy_from_user(dst, iov[i].iov_base, n))
			break;
		dst += n;
		left -= n;
	}
	r->payload_len = (u32)(sizeof(r->payload) - left);
}

static u64 a52_r359_writev_enter(struct pt_regs *regs, int scno)
{
	struct a52_r359_record r;
	u64 sequence;
	unsigned int pos;

	if (current->pid != 1 || scno != __NR_writev ||
	    !READ_ONCE(a52_r359_sideband))
		return 0;

	sequence = ++a52_r359_sequence;
	pos = (unsigned int)((sequence - 1ULL) % A52_R359_RING_SLOTS);
	memset(&r, 0, sizeof(r));
	r.magic = A52_R359_MAGIC;
	r.ns = ktime_get_ns();
	r.sequence = sequence;
	r.pc = regs->pc;
	r.lr = regs->regs[30];
	r.sp = regs->sp;
	r.ret = (s64)(1ULL << 63);
	r.event = A52_R359_EVT_ENTRY;
	r.fd = (s32)regs->orig_x0;
	r.cpu = (u32)raw_smp_processor_id();
	r.pid = (u32)current->pid;
	r.tgid = (u32)current->tgid;
	r.ring_index = pos;
	r.commit = A52_R359_COMMIT;
	r.version = A52_R359_VERSION;
	memcpy(r.comm, current->comm, TASK_COMM_LEN);
	a52_r359_copy_payload(&r, regs);
	memcpy(&a52_r359_ring[pos], &r, sizeof(r));
	if (a52_r359_count < A52_R359_RING_SLOTS)
		a52_r359_count++;
	return sequence;
}

static void a52_r359_writev_return(struct pt_regs *regs, u64 sequence)
{
	unsigned int pos;
	struct a52_r359_record *r;

	if (!sequence || current->pid != 1)
		return;
	pos = (unsigned int)((sequence - 1ULL) % A52_R359_RING_SLOTS);
	r = &a52_r359_ring[pos];
	if (READ_ONCE(r->sequence) != sequence ||
	    READ_ONCE(r->commit) != A52_R359_COMMIT)
		return;
	WRITE_ONCE(r->ret, (s64)regs->regs[0]);
	WRITE_ONCE(r->event, A52_R359_EVT_RETURN);
}

static void a52_r359_write_slot(unsigned int slot,
				const struct a52_r359_record *r)
{
	unsigned int pos = slot * A52_R359_SLOT_BYTES;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r359_sideband) || !r ||
	    slot >= A52_R359_SLOTS_PER_COPY)
		return;
	dst0 = (u8 *)a52_r359_sideband + pos;
	dst1 = (u8 *)a52_r359_sideband + A52_R359_COPY_BYTES + pos;
	memcpy(dst0, r, sizeof(*r));
	memcpy(dst1, r, sizeof(*r));
}

static void a52_r359_persist(void)
{
	u64 last;
	u64 first;
	u64 seq;
	unsigned int count;
	unsigned int i;
	unsigned int rep;
	unsigned int pos;
	const struct a52_r359_record *r;

	if (!READ_ONCE(a52_r359_sideband))
		return;
	count = READ_ONCE(a52_r359_count);
	if (!count)
		return;
	if (count > A52_R359_RING_SLOTS)
		count = A52_R359_RING_SLOTS;

	last = READ_ONCE(a52_r359_sequence);
	first = last - count + 1ULL;
	for (i = 0; i < count; i++) {
		seq = first + i;
		pos = (unsigned int)((seq - 1ULL) % A52_R359_RING_SLOTS);
		r = &a52_r359_ring[pos];
		if (READ_ONCE(r->sequence) != seq ||
		    READ_ONCE(r->commit) != A52_R359_COMMIT)
			continue;
		for (rep = 0; rep < A52_R359_REPLICAS; rep++)
			a52_r359_write_slot(i * A52_R359_REPLICAS + rep, r);
	}
	wmb();
	__flush_dcache_area(a52_r359_sideband, A52_R359_SIDEBAND_BYTES);
}

static void a52_r359_maybe_persist(int scno)
{
	if (current->pid == 1 &&
	    (scno == __NR_exit_group || scno == __NR_exit))
		a52_r359_persist();
}

static int __init __used a52_r359_init(void)
{
	BUILD_BUG_ON(sizeof(struct a52_r359_record) != A52_R359_SLOT_BYTES);
	BUILD_BUG_ON(A52_R359_SLOTS_PER_COPY * A52_R359_SLOT_BYTES !=
		     A52_R359_COPY_BYTES);
	BUILD_BUG_ON(A52_R359_RING_SLOTS * A52_R359_REPLICAS !=
		     A52_R359_SLOTS_PER_COPY);

	a52_r359_sideband = memremap(A52_R359_SIDEBAND_PHYS,
		A52_R359_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r359_sideband)
		return 0;
	memset(a52_r359_sideband, 0, A52_R359_SIDEBAND_BYTES);
	memset(a52_r359_ring, 0, sizeof(a52_r359_ring));
	a52_r359_sequence = 0;
	a52_r359_count = 0;
	wmb();
	__flush_dcache_area(a52_r359_sideband, A52_R359_SIDEBAND_BYTES);
	return 0;
}
/* Phase364 owns 0xB1BF0000..0xB1BF7FFF at runtime. */

static const char a52_r358_marker[] __used = "A52_PHASE358_PID1_SYSCALL_TRACE_V1";
static void *a52_r358_sideband;
static struct a52_r358_record a52_r358_ring[A52_R358_HISTORY_SLOTS];
static u64 a52_r358_sequence;
static u32 a52_r358_count;

static void a52_r358_fill(struct a52_r358_record *r,
			  struct pt_regs *regs, int scno, u32 event,
			  u64 sequence, s64 ret)
{
	struct mm_struct *mm = current->mm;
	unsigned int i;

	memset(r, 0, sizeof(*r));
	r->magic = A52_R358_MAGIC;
	r->ns = ktime_get_ns();
	r->sequence = sequence;
	r->pc = regs->pc;
	r->sp = regs->sp;
	r->pstate = regs->pstate;
	r->fp = regs->regs[29];
	r->lr = regs->regs[30];
	r->x[0] = regs->orig_x0;
	for (i = 1; i <= 8; i++)
		r->x[i] = regs->regs[i];
	r->mm = (u64)(unsigned long)mm;
	if (mm) {
		r->start_code = READ_ONCE(mm->start_code);
		r->end_code = READ_ONCE(mm->end_code);
		r->start_data = READ_ONCE(mm->start_data);
		r->end_data = READ_ONCE(mm->end_data);
		r->start_stack = READ_ONCE(mm->start_stack);
	}
	r->ret = ret;
	r->event = event;
	r->syscallno = (u32)scno;
	r->cpu = (u32)raw_smp_processor_id();
	r->pid = (u32)current->pid;
	r->tgid = (u32)current->tgid;
	r->ring_index = (u32)((sequence - 1ULL) % A52_R358_HISTORY_SLOTS);
	r->commit = A52_R358_COMMIT;
	r->version = A52_R358_VERSION;
	memcpy(r->comm, current->comm, TASK_COMM_LEN);
}

static void a52_r358_write_slot(unsigned int slot,
				const struct a52_r358_record *r)
{
	unsigned int pos = slot * A52_R358_SLOT_BYTES;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r358_sideband) || !r ||
	    slot >= A52_R358_SLOTS_PER_COPY)
		return;

	dst0 = (u8 *)a52_r358_sideband + pos;
	dst1 = (u8 *)a52_r358_sideband + A52_R358_COPY_BYTES + pos;
	memcpy(dst0, r, sizeof(*r));
	memcpy(dst1, r, sizeof(*r));
}

static void a52_r358_persist(const struct a52_r358_record *final)
{
	u64 first_sequence;
	u64 sequence;
	u32 count;
	unsigned int i;
	unsigned int pos;
	const struct a52_r358_record *src;

	if (!READ_ONCE(a52_r358_sideband) || !final)
		return;

	count = READ_ONCE(a52_r358_count);
	if (count > A52_R358_HISTORY_SLOTS)
		count = A52_R358_HISTORY_SLOTS;
	first_sequence = final->sequence - (u64)count + 1ULL;

	/* Slots 0..31: chronological syscall history, oldest to newest. */
	for (i = 0; i < count; i++) {
		sequence = first_sequence + (u64)i;
		pos = (unsigned int)((sequence - 1ULL) % A52_R358_HISTORY_SLOTS);
		src = &a52_r358_ring[pos];
		if (READ_ONCE(src->sequence) == sequence &&
		    READ_ONCE(src->commit) == A52_R358_COMMIT)
			a52_r358_write_slot(i, src);
	}

	/* Slots 32..63: replicated non-returning exit/exit_group entry. */
	for (i = A52_R358_FINAL_FIRST; i < A52_R358_SLOTS_PER_COPY; i++)
		a52_r358_write_slot(i, final);

	wmb();
	__flush_dcache_area(a52_r358_sideband, A52_R358_SIDEBAND_BYTES);
}

static u64 a52_r358_sys_enter(struct pt_regs *regs, int scno)
{
	struct a52_r358_record r;
	u64 sequence;
	unsigned int pos;
	u32 event = A52_R358_EVT_ENTRY;

	if (current->pid != 1 || !READ_ONCE(a52_r358_sideband))
		return 0;

	sequence = ++a52_r358_sequence;
	pos = (unsigned int)((sequence - 1ULL) % A52_R358_HISTORY_SLOTS);
	if (scno == __NR_exit_group || scno == __NR_exit)
		event = A52_R358_EVT_FINAL_EXIT;

	a52_r358_fill(&r, regs, scno, event, sequence,
			(s64)(1ULL << 63));
	memcpy(&a52_r358_ring[pos], &r, sizeof(r));
	if (a52_r358_count < A52_R358_HISTORY_SLOTS)
		a52_r358_count++;

	if (event == A52_R358_EVT_FINAL_EXIT)
		a52_r358_persist(&r);

	return sequence;
}

static void a52_r358_sys_return(struct pt_regs *regs, int scno, u64 sequence)
{
	struct a52_r358_record r;
	unsigned int pos;

	if (!sequence || current->pid != 1)
		return;

	pos = (unsigned int)((sequence - 1ULL) % A52_R358_HISTORY_SLOTS);
	if (READ_ONCE(a52_r358_ring[pos].sequence) != sequence)
		return;

	a52_r358_fill(&r, regs, scno, A52_R358_EVT_RETURN, sequence,
			(s64)regs->regs[0]);
	memcpy(&a52_r358_ring[pos], &r, sizeof(r));
}

static int __init __used a52_r358_init(void)
{
	BUILD_BUG_ON(sizeof(struct a52_r358_record) != A52_R358_SLOT_BYTES);
	BUILD_BUG_ON(A52_R358_SLOTS_PER_COPY * A52_R358_SLOT_BYTES !=
		     A52_R358_COPY_BYTES);
	BUILD_BUG_ON(A52_R358_HISTORY_SLOTS != A52_R358_FINAL_FIRST);

	a52_r358_sideband = memremap(A52_R358_SIDEBAND_PHYS,
		A52_R358_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r358_sideband)
		return 0;

	memset(a52_r358_sideband, 0, A52_R358_SIDEBAND_BYTES);
	memset(a52_r358_ring, 0, sizeof(a52_r358_ring));
	a52_r358_sequence = 0;
	a52_r358_count = 0;
	wmb();
	__flush_dcache_area(a52_r358_sideband, A52_R358_SIDEBAND_BYTES);
	return 0;
}
/* Phase359 owns 0xB1BF0000..0xB1BF7FFF at runtime. */


static long do_ni_syscall(struct pt_regs *regs, int scno)
{
#ifdef CONFIG_COMPAT
	long ret;
	if (is_compat_task()) {
		ret = compat_arm_syscall(regs, scno);
		if (ret != -ENOSYS)
			return ret;
	}
#endif

	return sys_ni_syscall();
}

static long __invoke_syscall(struct pt_regs *regs, syscall_fn_t syscall_fn)
{
	return syscall_fn(regs);
}

static void invoke_syscall(struct pt_regs *regs, unsigned int scno,
			   unsigned int sc_nr,
			   const syscall_fn_t syscall_table[])
{
	long ret;

	if (scno < sc_nr) {
		syscall_fn_t syscall_fn;
		syscall_fn = syscall_table[array_index_nospec(scno, sc_nr)];
		ret = __invoke_syscall(regs, syscall_fn);
	} else {
		ret = do_ni_syscall(regs, scno);
	}

	if (ret < 0 &&
	    (!strncmp(current->comm, "bootanimation", 13) ||
	     (current->group_leader &&
	      !strncmp(current->group_leader->comm, "bootanimation", 13))))
		a52_p440_event("BA_SYSCALL", current->comm, scno, ret);
	syscall_set_return_value(current, regs, 0, ret);
}

static inline bool has_syscall_work(unsigned long flags)
{
	return unlikely(flags & _TIF_SYSCALL_WORK);
}

int syscall_trace_enter(struct pt_regs *regs);
void syscall_trace_exit(struct pt_regs *regs);

#ifdef CONFIG_ARM64_ERRATUM_1463225
DECLARE_PER_CPU(int, __in_cortex_a76_erratum_1463225_wa);

static void cortex_a76_erratum_1463225_svc_handler(void)
{
	u32 reg, val;

	if (!unlikely(test_thread_flag(TIF_SINGLESTEP)))
		return;

	if (!unlikely(this_cpu_has_cap(ARM64_WORKAROUND_1463225)))
		return;

	__this_cpu_write(__in_cortex_a76_erratum_1463225_wa, 1);
	reg = read_sysreg(mdscr_el1);
	val = reg | DBG_MDSCR_SS | DBG_MDSCR_KDE;
	write_sysreg(val, mdscr_el1);
	asm volatile("msr daifclr, #8");
	isb();

	/* We will have taken a single-step exception by this point */

	write_sysreg(reg, mdscr_el1);
	__this_cpu_write(__in_cortex_a76_erratum_1463225_wa, 0);
}
#else
static void cortex_a76_erratum_1463225_svc_handler(void) { }
#endif /* CONFIG_ARM64_ERRATUM_1463225 */

static void el0_svc_common(struct pt_regs *regs, int scno, int sc_nr,
			   const syscall_fn_t syscall_table[])
{
	unsigned long flags = current_thread_info()->flags;
	u64 a52_r358_sequence_token = 0;
	u64 a52_r359_sequence_token = 0;
	u64 a52_r364_sequence_token = 0;
	u64 a52_r365_sequence_token = 0;
	u64 a52_r366_sequence_token = 0;
	u64 a52_r368_sequence_token = 0;
	u64 a52_r369_sequence_token = 0;
	u64 a52_r370_sequence_token = 0;
	u64 a52_r371_sequence_token = 0;
	u64 a52_r372_main_sequence_token = 0;
	u64 a52_r373_mount_token = 0;

	regs->orig_x0 = regs->regs[0];
	regs->syscallno = scno;

	/*
	 * BTI note:
	 * The architecture does not guarantee that SPSR.BTYPE is zero
	 * on taking an SVC, so we could return to userspace with a
	 * non-zero BTYPE after the syscall.
	 *
	 * This shouldn't matter except when userspace is explicitly
	 * doing something stupid, such as setting PROT_BTI on a page
	 * that lacks conforming BTI/PACIxSP instructions, falling
	 * through from one executable page to another with differing
	 * PROT_BTI, or messing with BTYPE via ptrace: in such cases,
	 * userspace should not be surprised if a SIGILL occurs on
	 * syscall return.
	 *
	 * So, don't touch regs->pstate & PSR_BTYPE_MASK here.
	 * (Similarly for HVC and SMC elsewhere.)
	 */

	cortex_a76_erratum_1463225_svc_handler();
	local_daif_restore(DAIF_PROCCTX);

	if (flags & _TIF_MTE_ASYNC_FAULT) {
		/*
		 * Process the asynchronous tag check fault before the actual
		 * syscall. do_notify_resume() will send a signal to userspace
		 * before the syscall is restarted.
		 */
		syscall_set_return_value(current, regs, -ERESTARTNOINTR, 0);
		return;
	}

	if (has_syscall_work(flags)) {
		/*
		 * The de-facto standard way to skip a system call using ptrace
		 * is to set the system call to -1 (NO_SYSCALL) and set x0 to a
		 * suitable error code for consumption by userspace. However,
		 * this cannot be distinguished from a user-issued syscall(-1)
		 * and so we must set x0 to -ENOSYS here in case the tracer doesn't
		 * issue the skip and we fall into trace_exit with x0 preserved.
		 *
		 * This is slightly odd because it also means that if a tracer
		 * sets the system call number to -1 but does not initialise x0,
		 * then x0 will be preserved for all system calls apart from a
		 * user-issued syscall(-1). However, requesting a skip and not
		 * setting the return value is unlikely to do anything sensible
		 * anyway.
		 */
		if (scno == NO_SYSCALL)
			syscall_set_return_value(current, regs, -ENOSYS, 0);
		scno = syscall_trace_enter(regs);
		if (scno == NO_SYSCALL)
			goto trace_exit;
	}

	a52_r359_maybe_persist(scno);
	a52_r359_sequence_token = a52_r359_writev_enter(regs, scno);
	a52_r358_sequence_token = a52_r358_sys_enter(regs, scno);
	a52_r364_sequence_token = a52_r364_sys_enter(regs, scno);
	a52_r365_sequence_token = a52_r365_sys_enter(regs, scno);
	a52_r366_sequence_token = a52_r366_sys_enter(regs, scno);
	a52_r368_sequence_token = a52_r368_sys_enter(regs, scno);
	a52_r369_maybe_init_exec(regs, scno);
	a52_r369_sequence_token = a52_r369_main_enter(regs, scno);
	a52_r370_sequence_token = a52_r370_worker_enter(regs, scno);
	a52_r371_sequence_token = a52_r371_worker_enter(regs, scno);
	a52_r372_main_sequence_token = a52_r372_main_wait_enter(regs, scno);
	a52_r372_worker_enter(regs, scno);
	a52_r373_mount_token = a52_r373_mount_enter(regs, scno);
	if (unlikely(current->pid == 1 && scno == __NR_exit_group)) {
		u64 a52s = ((u64)(u32)READ_ONCE(current->signal->flags) << 32) |
			(u32)READ_ONCE(current->signal->group_exit_code);
		a52_p435_mark(P435_I0_EXIT_GROUP_SYSCALL, regs->regs[0], a52s, regs);
		a52_p436_mark(P436_I0_EXIT_GROUP_SYSCALL, regs->regs[0], a52s, regs);
	}
	invoke_syscall(regs, scno, sc_nr, syscall_table);
	if ((!strcmp(current->comm, "odsign") ||
	     !strcmp(current->comm, "odrefresh")) &&
	    (scno == __NR_add_key || scno == __NR_keyctl))
		a52_ackfr_record("P434 ART KEY sc=%d op=%llx rc=%ld",
			scno, scno == __NR_keyctl ?
			(unsigned long long)regs->orig_x0 : 0ULL,
			(long)regs->regs[0]);
	a52_r370_note_clone_return(regs, scno);
	a52_r371_note_clone_return(regs, scno);
	a52_r372_note_clone_return(regs, scno);
	a52_r373_note_clone_return(regs, scno);
	a52_r373_mount_return(regs, scno, a52_r373_mount_token);
	a52_r372_worker_return(regs, scno);
	a52_r372_main_wait_return(regs, scno, a52_r372_main_sequence_token);
	a52_r371_worker_return(regs, scno, a52_r371_sequence_token);
	a52_r370_worker_return(regs, scno, a52_r370_sequence_token);
	a52_r369_main_return(regs, scno, a52_r369_sequence_token);
	a52_r368_sys_return(regs, scno, a52_r368_sequence_token);
	a52_r366_sys_return(regs, scno, a52_r366_sequence_token);
	a52_r365_sys_return(regs, scno, a52_r365_sequence_token);
	a52_r364_sys_return(regs, scno, a52_r364_sequence_token);
	a52_r358_sys_return(regs, scno, a52_r358_sequence_token);
	a52_r359_writev_return(regs, a52_r359_sequence_token);

	/*
	 * The tracing status may have changed under our feet, so we have to
	 * check again. However, if we were tracing entry, then we always trace
	 * exit regardless, as the old entry assembly did.
	 */
	if (!has_syscall_work(flags) && !IS_ENABLED(CONFIG_DEBUG_RSEQ)) {
		local_daif_mask();
		flags = current_thread_info()->flags;
		if (!has_syscall_work(flags) && !(flags & _TIF_SINGLESTEP))
			return;
		local_daif_restore(DAIF_PROCCTX);
	}

trace_exit:
	syscall_trace_exit(regs);
}

static inline void sve_user_discard(void)
{
	if (!system_supports_sve())
		return;

	clear_thread_flag(TIF_SVE);

	/*
	 * task_fpsimd_load() won't be called to update CPACR_EL1 in
	 * ret_to_user unless TIF_FOREIGN_FPSTATE is still set, which only
	 * happens if a context switch or kernel_neon_begin() or context
	 * modification (sigreturn, ptrace) intervenes.
	 * So, ensure that CPACR_EL1 is already correct for the fast-path case.
	 */
	sve_user_disable();
}

void do_el0_svc(struct pt_regs *regs)
{
	sve_user_discard();
	el0_svc_common(regs, regs->regs[8], __NR_syscalls, sys_call_table);
}

#ifdef CONFIG_COMPAT
void do_el0_svc_compat(struct pt_regs *regs)
{
	el0_svc_common(regs, regs->regs[7], __NR_compat_syscalls,
		       compat_sys_call_table);
}
#endif

/* A52_PHASE430_HARDENING_V2: natural task alignment + atomic endpoint wake + task-copy dsb. */

/* A52_PHASE431_SAFE_SF_GAP_FORENSICS_V1: no UFS sampling; immediate/gap snapshots + CNTPCT witness. */

/* A52_PHASE432_SF_HWC_BOOTANIM_CHAIN_V1: light SF+0; real HWC owner + bootanimation frontier. */

/* A52_PHASE434_CLEAN_F0_V1: P430/P431/P432 periodic task sampler removed. */

/* A52_PHASE435_TERMINAL_RACE_V1: PID1 exit_group syscall entry. */

/* A52_PHASE436_ADRENO_START_FRONTIER_V1: PID1 syscall lane. */

/* A52_PHASE440_EARLY_F0_CLEANUP_FORENSICS_V1: failed bootanimation syscalls captured. */

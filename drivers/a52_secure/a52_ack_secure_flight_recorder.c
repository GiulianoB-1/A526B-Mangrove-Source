// SPDX-License-Identifier: GPL-2.0-only
/*
 * A52 GKI 5.10 first-atomic recorder, phase 210.
 *
 * Records are fixed-size, CRC32C validated, Reed-Solomon protected, and written
 * independently to the record, console and ftrace RAMOOPS banks. Payloads
 * remain metadata-only.
 */
#undef pr_fmt
#define pr_fmt(fmt) "A52R210: " fmt

#include <linux/atomic.h>
#include <linux/byteorder/generic.h>
#include <linux/cpu.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/errno.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/sizes.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/hrtimer.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/suspend.h>
#include <linux/math64.h>
#include <asm/cacheflush.h>
#include <asm/barrier.h>
#include <linux/rslib.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/stat.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/utsname.h>
#include <linux/workqueue.h>

#include <linux/a52_ack_secure_flight_recorder.h>

#define A52_R179_CAPACITY 896U
#define A52_R179_MESSAGE_LEN 90U
#define A52_R179_BANK_CONSOLE BIT(0)
#define A52_R179_BANK_FTRACE BIT(1)
#define A52_R179_BANK_RECORD BIT(2)
#define A52_R179_BANK_ALL (A52_R179_BANK_CONSOLE | \
			   A52_R179_BANK_FTRACE | A52_R179_BANK_RECORD)
#define A52_R179_COMMIT 0x5a52c210U
#define A52_R179_VERSION 3U
#define A52_R179_RS_ROOTS 48U
#define A52_R179_DATA_BYTES 141U
#define A52_R179_CODE_BYTES (A52_R179_DATA_BYTES + A52_R179_RS_ROOTS)
#define A52_R179_TEXT_BYTES 255U
#define A52_R179_PREFIX "R48"
#define A52_R179_PREFIX_BYTES 3U
#define A52_R210_PACKED_MESSAGE_LEN 73U

struct a52_r179_event {
	u64 seq;
	u64 monotonic_ns;
	pid_t pid;
	pid_t tgid;
	u32 cpu;
	u8 persisted_mask;
	char comm[TASK_COMM_LEN];
	char message[A52_R179_MESSAGE_LEN];
};

struct a52_r179_data {
	u8 magic[8];
	__le16 version;
	__le16 header_len;
	__le64 seq;
	__le64 monotonic_ns;
	__le32 pid;
	__le32 tgid;
	__le32 cpu;
	__le16 kind;
	__le16 message_len;
	char comm[TASK_COMM_LEN];
	char message[A52_R210_PACKED_MESSAGE_LEN];
	__le32 crc32c;
	__le32 commit;
} __packed;

static struct a52_r179_event a52_r179_events[A52_R179_CAPACITY];
static DEFINE_SPINLOCK(a52_r179_lock);
static DEFINE_RAW_SPINLOCK(a52_r179_rs_lock);
static atomic64_t a52_r179_sequence = ATOMIC64_INIT(0);
static atomic64_t a52_r179_dropped = ATOMIC64_INIT(0);
static atomic_t a52_r179_backend_seen = ATOMIC_INIT(0);
/* A52_PHASE280_TIMEOUT_RETENTION_LATCH_V1 */
static atomic_t a52_r280_retained = ATOMIC_INIT(0);
static atomic_t a52_p418_gate_drop = ATOMIC_INIT(0);

unsigned int a52_ackfr_phase418_retained(void)
{
	return (unsigned int)atomic_read(&a52_r280_retained);
}
EXPORT_SYMBOL_GPL(a52_ackfr_phase418_retained);

static void a52_p418_retention_drop_note(const char *fmt)
{
	unsigned int n;

	if (!fmt)
		return;
	n = (unsigned int)atomic_inc_return(&a52_p418_gate_drop);
	if (n <= 16U || !(n & 127U))
		a52_ackfr_record("P418 GATE n=%u ret=1 f=%.9s", n, fmt);
}
static struct rs_control *a52_r179_rs;


static u32 a52_r199_crc32c(const void *buffer, size_t len)
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

extern unsigned int a52_ackfr_ramoops_write(const char *buf, size_t len,
					     unsigned int targets);

static int a52_r179_base64_encode(const u8 *src, size_t len, char *dst)
{
	static const char table[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i;
	int out = 0;

	if (!src || !dst || (len % 3))
		return -EINVAL;
	for (i = 0; i < len; i += 3) {
		u32 value = ((u32)src[i] << 16) | ((u32)src[i + 1] << 8) |
			src[i + 2];

		dst[out++] = table[(value >> 18) & 0x3f];
		dst[out++] = table[(value >> 12) & 0x3f];
		dst[out++] = table[(value >> 6) & 0x3f];
		dst[out++] = table[value & 0x3f];
	}
	return out;
}

static void a52_r179_pack(const struct a52_r179_event *event,
			  struct a52_r179_data *data)
{
	size_t message_len;

	memset(data, 0, sizeof(*data));
	memcpy(data->magic, "A52R0210", sizeof(data->magic));
	data->version = cpu_to_le16(A52_R179_VERSION);
	data->header_len = cpu_to_le16(60U);
	data->seq = cpu_to_le64(event->seq);
	data->monotonic_ns = cpu_to_le64(event->monotonic_ns);
	data->pid = cpu_to_le32((u32)event->pid);
	data->tgid = cpu_to_le32((u32)event->tgid);
	data->cpu = cpu_to_le32(event->cpu);
	data->kind = cpu_to_le16(0U);
	message_len = strnlen(event->message, sizeof(data->message));
	data->message_len = cpu_to_le16((u16)message_len);
	memcpy(data->comm, event->comm, sizeof(data->comm));
	memcpy(data->message, event->message, message_len);
	data->crc32c = cpu_to_le32(a52_r199_crc32c(data,
			offsetof(struct a52_r179_data, crc32c)));
	data->commit = cpu_to_le32(A52_R179_COMMIT);
}

static unsigned int a52_r179_persist_event(const struct a52_r179_event *event,
					    unsigned int targets)
{
	struct a52_r179_data data;
	u16 parity[A52_R179_RS_ROOTS];
	u8 codeword[A52_R179_CODE_BYTES];
	char text[A52_R179_TEXT_BYTES + 1];
	unsigned long irq_flags;
	unsigned int written;
	int encoded;
	int i;

	if (!targets || !READ_ONCE(a52_r179_rs))
		return 0;

	BUILD_BUG_ON(sizeof(struct a52_r179_data) != A52_R179_DATA_BYTES);
	BUILD_BUG_ON(A52_R179_CODE_BYTES != 189U);
	a52_r179_pack(event, &data);
	memset(parity, 0, sizeof(parity));
	memcpy(codeword, &data, sizeof(data));

	raw_spin_lock_irqsave(&a52_r179_rs_lock, irq_flags);
	encoded = encode_rs8(a52_r179_rs, codeword, A52_R179_DATA_BYTES,
			     parity, 0);
	if (encoded) {
		raw_spin_unlock_irqrestore(&a52_r179_rs_lock, irq_flags);
		return 0;
	}
	for (i = 0; i < A52_R179_RS_ROOTS; i++)
		codeword[A52_R179_DATA_BYTES + i] = (u8)parity[i];
	memcpy(text, A52_R179_PREFIX, A52_R179_PREFIX_BYTES);
	encoded = a52_r179_base64_encode(codeword, sizeof(codeword),
				text + A52_R179_PREFIX_BYTES);
	if (encoded != A52_R179_TEXT_BYTES - A52_R179_PREFIX_BYTES) {
		raw_spin_unlock_irqrestore(&a52_r179_rs_lock, irq_flags);
		return 0;
	}
	text[A52_R179_TEXT_BYTES] = '\0';
	written = a52_ackfr_ramoops_write(text, A52_R179_TEXT_BYTES, targets);
	raw_spin_unlock_irqrestore(&a52_r179_rs_lock, irq_flags);
	return written;
}

static void a52_r179_flush_missing(u64 through)
{
	struct a52_r179_event event;
	unsigned long irq_flags;
	unsigned int missing;
	unsigned int written;
	u64 cursor;

	if (through > A52_R179_CAPACITY)
		through = A52_R179_CAPACITY;
	for (cursor = 1; cursor <= through; cursor++) {
		spin_lock_irqsave(&a52_r179_lock, irq_flags);
		event = a52_r179_events[cursor - 1];
		spin_unlock_irqrestore(&a52_r179_lock, irq_flags);
		if (event.seq != cursor)
			continue;
		missing = A52_R179_BANK_ALL & ~event.persisted_mask;
		if (!missing)
			continue;
		written = a52_r179_persist_event(&event, missing);
		if (!written)
			continue;
		spin_lock_irqsave(&a52_r179_lock, irq_flags);
		a52_r179_events[cursor - 1].persisted_mask |= written;
		spin_unlock_irqrestore(&a52_r179_lock, irq_flags);
	}
}

static bool a52_r179_is_critical_message(const char *message)
{
	if (!message)
		return false;

	/* A52_PHASE241_CXF241_POSTCAPACITY_CRITICAL_V1
	 * Keep Phase 241 late replay visible after A52_R179_CAPACITY is exhausted.
	 */
	/* A52_PHASE267_PREDRM_DIRECT_BOUNDARY_V3: dedicated diagnostics only. */
/* A52_PHASE268_COMPOSER_DRM_STICKY_V1
 * DRMPOST 211/212 are admitted only so sticky state can consume them before
 * capacity suppression.  Keep those raw events noncritical to protect the
 * late retained window.  Only compact P268 snapshots are critical.
 */
if (!strncmp(message, "DRMPOST 211", 11) ||
    !strncmp(message, "DRMPOST 212", 11))
	return false;
/* A52_PHASE446_ACK_ADMISSION */
return !strncmp(message, "P446 ", 5) ||
       !strncmp(message, "P440 ", 5) ||
       !strncmp(message, "P439 ", 5) ||
       !strncmp(message, "P434 ", 5) ||
	       !strncmp(message, "P432 ", 5) ||
	       !strncmp(message, "P431 ", 5) ||
	       !strncmp(message, "P430 ", 5) ||
	       !strncmp(message, "P429 ", 5) ||
	       !strncmp(message, "P427 ", 5) ||
	       !strncmp(message, "P426 ", 5) ||
	       !strncmp(message, "P425 ", 5) ||
	       !strncmp(message, "P414 ", 5) ||
       !strncmp(message, "P276 ", 5) ||
       !strncmp(message, "P275 ", 5) ||
       !strncmp(message, "P274 ", 5) ||
       !strncmp(message, "P273 ", 5) ||
       !strncmp(message, "P271 ", 5) ||
       !strncmp(message, "P270 ", 5) ||
       !strncmp(message, "P269 ", 5) ||
       !strncmp(message, "P268 ", 5) ||
       !strncmp(message, "P267 ", 5) ||
       !strncmp(message, "F261 ", 5) ||
       !strncmp(message, "F260 ", 5) ||
       !strncmp(message, "F259 ", 5) ||
       !strncmp(message, "F257 ", 5) ||
       !strncmp(message, "F256 ", 5) ||
       !strncmp(message, "K255VIS ", 8) ||
       !strncmp(message, "TRIPOST ", 8) ||
       !strncmp(message, "BOOTPOST ", 9) ||
       !strncmp(message, "USRPOST ", 8) ||
       !strncmp(message, "ODSPOST ", 8) ||
       !strncmp(message, "GFXPOST ", 8) ||
       !strncmp(message, "K254 ", 5) ||
       !strncmp(message, "K253 ", 5) ||
       !strncmp(message, "K251 ", 5) ||
       !strncmp(message, "K250 ", 5) ||
       !strncmp(message, "K249 ", 5) ||
       !strncmp(message, "K248 ", 5) ||
       !strncmp(message, "CXF246 ", 7) ||
	       !strncmp(message, "CXF243 ", 7) ||
	       !strncmp(message, "CXF242 ", 7) ||
	       !strncmp(message, "CXF241 ", 7) ||
	       !strncmp(message, "BOOT ", 5) ||
	       !strncmp(message, "HB ", 3) ||
	       !strncmp(message, "REFGEN ", 7) ||
	       !strncmp(message, "DISP ", 5) ||
	       !strncmp(message, "WDT ", 4) ||
	       !strncmp(message, "DRMPOST ", 8) ||
	       !strncmp(message, "GFXPOST ", 8) ||
	       !strncmp(message, "KEYPOST ", 8) ||
	       !strncmp(message, "IONPOST ", 8) ||
	       !strncmp(message, "SGPOST ", 7) ||
	       !strncmp(message, "UFPOST ", 7) ||
	       !strncmp(message, "BOOTPOST ", 9) ||
	       !strncmp(message, "TRIPOST ", 8) ||
	       !strncmp(message, "KGPPOST ", 8) ||
	       !strncmp(message, "ODSPOST ", 8) ||
	       !strncmp(message, "KMSPOST ", 8) ||
	       !strncmp(message, "KMSBLK ", 7) ||
	       !strncmp(message, "CAT ", 4) ||
	       !strncmp(message, "SMMU ", 5) ||
	       !strncmp(message, "DCORE ", 6) ||
	       !strncmp(message, "DLINK ", 6) ||
	       !strncmp(message, "DMA ", 4) ||
	       !strncmp(message, "IOMMU ", 6) ||
	       !strncmp(message, "A52GDSC ", 8) ||
	       !strncmp(message, "U380 ", 5) ||
	       !strncmp(message, "B380 ", 5) ||
	       !strncmp(message, "L380 ", 5) ||
	       !strncmp(message, "USB380 ", 7) ||
	       !strncmp(message, "V380 ", 5) ||
	       !strncmp(message, "V381 ", 5) ||
	       !strncmp(message, "V382 ", 5) ||
	       !strncmp(message, "P383 ", 5) ||
	       !strncmp(message, "L383 ", 5) ||
	       !strncmp(message, "U384 ", 5) ||
	       !strncmp(message, "B384 ", 5) ||
	       !strncmp(message, "L384 ", 5) ||
	       !strncmp(message, "P385 ", 5) ||
	       !strncmp(message, "B385 ", 5) ||
	       !strncmp(message, "V385 ", 5); /* A52_PHASE381_RETENTION_V1 */
}

/* A52_PHASE228_TRI_TRACK_SNAPSHOT
 * Compact cumulative state survives ring overwrite and correlates the early
 * vold boundary, the odsign/odrefresh boundary, and the late SF/KGSL boundary.
 * Observation only: no return value, ordering, signal, or security behavior is
 * changed.
 */
static atomic_t a52_r228_v_stage = ATOMIC_INIT(0);
static atomic_t a52_r228_v_value = ATOMIC_INIT(0);
static atomic_t a52_r228_v_count = ATOMIC_INIT(0);
static atomic_t a52_r228_o_proc = ATOMIC_INIT(0);
static atomic_t a52_r228_o_stage = ATOMIC_INIT(0);
static atomic_t a52_r228_o_value = ATOMIC_INIT(0);
static atomic_t a52_r228_o_count = ATOMIC_INIT(0);
static atomic_t a52_r228_f_stage = ATOMIC_INIT(0);
static atomic_t a52_r228_f_value = ATOMIC_INIT(0);
static atomic_t a52_r228_f_count = ATOMIC_INIT(0);
static atomic_t a52_r228_g_open = ATOMIC_INIT(-61);
static atomic_t a52_r228_g_probe = ATOMIC_INIT(0);
static atomic_t a52_r228_g_reg = ATOMIC_INIT(0);
static atomic_t a52_r228_g_node = ATOMIC_INIT(0);

static bool a52_r228_has(const char *message, const char *needle)
{
	return message && needle && strnstr(message, needle, A52_R179_MESSAGE_LEN);
}

static int a52_r228_dec(const char *message, const char *key, int fallback)
{
	const char *value;
	int parsed;

	value = strnstr(message, key, A52_R179_MESSAGE_LEN);
	if (!value)
		return fallback;
	value += strlen(key);
	return sscanf(value, "%d", &parsed) == 1 ? parsed : fallback;
}


/* A52_PHASE273_LATE_BOOT_FRONTIER_RECORDER_V2
 * Phase272 hardware proved the DRM mode-loss blocker is gone. The retained
 * window then became dominated by a repeated successful P271 best-encoder
 * callback. Keep a compact late-boot frontier that survives that traffic.
 * Observation only: no DRM, DSI, panel, Binder, scheduler or userspace return
 * value is modified.
 */
#define A52_R273_FRONTIER_END_S 900U
#define A52_R273_SCAN_FAST_MS 2000U
#define A52_R273_SCAN_SLOW_MS 5000U
#define A52_R273_SUMMARY_S 15U
#define A52_R273_TASK_COUNT 14U

static atomic_t a52_r273_disp_count = ATOMIC_INIT(0);
static atomic_t a52_r273_kd_status = ATOMIC_INIT(-61);
static atomic_t a52_r273_mode_count = ATOMIC_INIT(-1);
static atomic_t a52_r273_encoder_id = ATOMIC_INIT(-1);
static atomic_t a52_r273_selected_id = ATOMIC_INIT(-1);
static DEFINE_SPINLOCK(a52_r273_disp_lock);
static char a52_r273_last_disp_fn[40];
static char a52_r273_last_disp_kind = '?';

static void a52_r273_track_message(const char *message)
{
	unsigned long flags;
	const char *fn;
	char kind;
	size_t len;

	if (!message)
		return;

	if (!strncmp(message, "DISP ", 5)) {
		fn = strnstr(message, "fn=", A52_R179_MESSAGE_LEN);
		if (fn) {
			fn += 3;
			kind = !strncmp(message, "DISP enter ", 11) ? 'E' :
			       (!strncmp(message, "DISP exit ", 10) ? 'X' : '?');
			len = strcspn(fn, " ");
			spin_lock_irqsave(&a52_r273_disp_lock, flags);
			strscpy(a52_r273_last_disp_fn, fn,
				min_t(size_t, sizeof(a52_r273_last_disp_fn), len + 1));
			a52_r273_last_disp_kind = kind;
			spin_unlock_irqrestore(&a52_r273_disp_lock, flags);
		}
		atomic_inc(&a52_r273_disp_count);
		return;
	}

	if (!strncmp(message, "P271 V ", 7) &&
	    strnstr(message, "k=D", A52_R179_MESSAGE_LEN)) {
		atomic_set(&a52_r273_kd_status,
			a52_r228_dec(message, "st=", atomic_read(&a52_r273_kd_status)));
	} else if (!strncmp(message, "P269 CONN ", 10)) {
		atomic_set(&a52_r273_mode_count,
			a52_r228_dec(message, "nm=", atomic_read(&a52_r273_mode_count)));
		atomic_set(&a52_r273_encoder_id,
			a52_r228_dec(message, "enc=", atomic_read(&a52_r273_encoder_id)));
	} else if (!strncmp(message, "P271 G ", 7)) {
		atomic_set(&a52_r273_selected_id,
			a52_r228_dec(message, "sel=", atomic_read(&a52_r273_selected_id)));
	}
}

static int a52_r228_hex(const char *message, const char *key, int fallback)
{
	const char *value;
	unsigned int parsed;

	value = strnstr(message, key, A52_R179_MESSAGE_LEN);
	if (!value)
		return fallback;
	value += strlen(key);
	return sscanf(value, "%x", &parsed) == 1 ? (int)parsed : fallback;
}

static int a52_r228_stage(const char *message)
{
	if (a52_r228_has(message, "exec-ret"))
		return 2;
	if (a52_r228_has(message, "exec "))
		return 1;
	if (a52_r228_has(message, "exit"))
		return 3;
	if (a52_r228_has(message, "open"))
		return 4;
	if (a52_r228_has(message, "io-in"))
		return 5;
	if (a52_r228_has(message, "io-out"))
		return 6;
	if (a52_r228_has(message, "con-in"))
		return 7;
	if (a52_r228_has(message, "con-out"))
		return 8;
	return 0;
}

static int a52_r228_value(const char *message, int fallback)
{
	int value;

	value = a52_r228_dec(message, "code=", fallback);
	if (value != fallback)
		return value;
	value = a52_r228_dec(message, "rc=", fallback);
	if (value != fallback)
		return value;
	value = a52_r228_dec(message, "r=", fallback);
	if (value != fallback)
		return value;
	return a52_r228_hex(message, "x=", fallback);
}

/* A52_PHASE267_PREDRM_STICKY_RETENTION_V1
 * Phase267R hardware showed that direct P267 records can execute before the
 * retained ramoops window and later be overwritten. Latch only compact
 * display-init / P267 state and re-emit it during the late heartbeat window.
 * Observation only: no display, DRM, probe, ordering or return-value change.
 */
/* A52_PHASE268_COMPOSER_DRM_STICKY_V1
 * Composer-specific state derived from already existing Phase211/212 events.
 * All fields are observation-only atomics so early evidence survives rollover.
 */
static atomic_t a52_r268_comp_pid = ATOMIC_INIT(-1);
static atomic_t a52_r268_exec_seen = ATOMIC_INIT(0);

/* A52_PHASE432_SF_HWC_BOOTANIM_CHAIN_V1
 * Phase268/269 guessed Composer from the executable path. Phase431 proved
 * that can latch the wrong QTI display service. The successful DRM primary
 * open is now the authoritative owner identity; old exec identity is fallback
 * only until the real owner is known.
 */
static atomic_t a52_p432_drm_owner_tgid = ATOMIC_INIT(-1);

void a52_ackfr_phase432_note_drm_owner(pid_t tgid)
{
	if (tgid > 0)
		atomic_cmpxchg(&a52_p432_drm_owner_tgid, -1, (int)tgid);
}
EXPORT_SYMBOL_GPL(a52_ackfr_phase432_note_drm_owner);

int a52_ackfr_phase432_drm_owner_tgid(void)
{
	return atomic_read(&a52_p432_drm_owner_tgid);
}
EXPORT_SYMBOL_GPL(a52_ackfr_phase432_drm_owner_tgid);

static atomic_t a52_r268_path_count = ATOMIC_INIT(0);
static atomic_t a52_r268_path_kind = ATOMIC_INIT(0);
static atomic_t a52_r268_path_fd = ATOMIC_INIT(-61);
static atomic_t a52_r268_path_pending = ATOMIC_INIT(0);
static atomic_t a52_r268_path_errors = ATOMIC_INIT(0);

static atomic_t a52_r268_drm_count = ATOMIC_INIT(0);
static atomic_t a52_r268_drm_stage = ATOMIC_INIT(0);
static atomic_t a52_r268_drm_minor = ATOMIC_INIT(-1);
static atomic_t a52_r268_drm_rc = ATOMIC_INIT(-61);
static atomic_t a52_r268_drm_pending = ATOMIC_INIT(0);
static atomic_t a52_r268_drm_errors = ATOMIC_INIT(0);

static atomic_t a52_r268_msm_open_count = ATOMIC_INIT(0);
static atomic_t a52_r268_msm_open_rc = ATOMIC_INIT(-61);
static atomic_t a52_r268_msm_open_pending = ATOMIC_INIT(0);
static atomic_t a52_r268_msm_open_errors = ATOMIC_INIT(0);

static atomic_t a52_r268_ioctl_count = ATOMIC_INIT(0);
static atomic_t a52_r268_ioctl_nr = ATOMIC_INIT(-1);
static atomic_t a52_r268_ioctl_rc = ATOMIC_INIT(-61);
static atomic_t a52_r268_ioctl_pending = ATOMIC_INIT(0);
static atomic_t a52_r268_ioctl_errors = ATOMIC_INIT(0);

static atomic_t a52_r268_atomic_count = ATOMIC_INIT(0);
static atomic_t a52_r268_atomic_rc = ATOMIC_INIT(-61);
static atomic_t a52_r268_atomic_pending = ATOMIC_INIT(0);
static atomic_t a52_r268_atomic_errors = ATOMIC_INIT(0);
static atomic_t a52_r268_close_count = ATOMIC_INIT(0);

static int a52_r268_hex(const char *message, const char *key, int fallback)
{
	const char *p;
	int value = 0;
	int digit;
	bool seen = false;

	if (!message || !key)
		return fallback;
	p = strstr(message, key);
	if (!p)
		return fallback;
	p += strlen(key);
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	while (*p) {
		if (*p >= '0' && *p <= '9')
			digit = *p - '0';
		else if (*p >= 'a' && *p <= 'f')
			digit = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F')
			digit = *p - 'A' + 10;
		else
			break;
		seen = true;
		value = (value << 4) | digit;
		p++;
	}
	return seen ? value : fallback;
}

static bool a52_r268_is_composer_pid(int pid)
{
	return pid >= 0 && pid == atomic_read(&a52_r268_comp_pid);
}

/* A52_PHASE269_EXACT_COMPOSER_TGID_V1
 * Reuse Phase268's exec-derived composer identity. This avoids task-comm
 * ambiguity (vendor.qti.hardware.* truncates to vendor.qti.hard).
 */
bool a52_ackfr_phase269_is_composer_tgid(pid_t tgid)
{
	int owner = atomic_read(&a52_p432_drm_owner_tgid);
	if (owner > 0)
		return tgid == owner;
	return a52_r268_is_composer_pid((int)tgid);
}
EXPORT_SYMBOL_GPL(a52_ackfr_phase269_is_composer_tgid);

static bool a52_r268_entry_is_composer(const char *message)
{
	int pid;

	pid = a52_r228_dec(message, "g=", -1);
	if (pid < 0)
		pid = a52_r228_dec(message, "pid=", -1);
	if (pid < 0)
		pid = a52_r228_dec(message, "p=", -1);
	return a52_r268_is_composer_pid(pid);
}

static int a52_r268_path_kind_of(const char *message)
{
	if (strstr(message, "/dev/dri/render"))
		return 2;
	if (strstr(message, "/dev/dri/card"))
		return 1;
	if (strstr(message, "/dev/dri/"))
		return 3;
	if (strstr(message, "/sys/class/drm/"))
		return 4;
	return 0;
}

static void a52_r268_track_drm(const char *message)
{
	int n;
	int rc;
	int kind;

	if (!message)
		return;

	/* Phase212 stores only the first 40 path bytes.  The QTI composer binary
	 * is already uniquely identifiable by this prefix before "composer". */
	if (!strncmp(message, "DRMPOST 212 exec ", 17) &&
	    (strstr(message, "vendor.qti.hardware.displ") ||
	     strstr(message, "composer"))) {
		atomic_set(&a52_r268_comp_pid, a52_r228_dec(message, "p=", -1));
		atomic_inc(&a52_r268_exec_seen);
		return;
	}

	if (!strncmp(message, "DRMPOST 212 path ", 17)) {
		kind = a52_r268_path_kind_of(message);
		if (kind && a52_r268_entry_is_composer(message)) {
			n = a52_r228_dec(message, "n=", 0);
			atomic_inc(&a52_r268_path_count);
			atomic_set(&a52_r268_path_pending, n);
			atomic_set(&a52_r268_path_kind, kind);
		}
		return;
	}
	if (!strncmp(message, "DRMPOST 212 path-ret ", 21)) {
		n = a52_r228_dec(message, "n=", 0);
		if (n && n == atomic_read(&a52_r268_path_pending)) {
			rc = a52_r228_dec(message, "fd=", -61);
			atomic_set(&a52_r268_path_fd, rc);
			atomic_set(&a52_r268_path_pending, 0);
			if (rc < 0)
				atomic_inc(&a52_r268_path_errors);
		}
		return;
	}

	if (!strncmp(message, "DRMPOST 212 drm-open ", 21)) {
		if (a52_r268_entry_is_composer(message)) {
			n = a52_r228_dec(message, "n=", 0);
			atomic_inc(&a52_r268_drm_count);
			atomic_set(&a52_r268_drm_pending, n);
			atomic_set(&a52_r268_drm_stage, 1);
			atomic_set(&a52_r268_drm_minor, a52_r228_dec(message, "id=", -1));
		}
		return;
	}
	if (!strncmp(message, "DRMPOST 212 drm-acquire ", 24) ||
	    !strncmp(message, "DRMPOST 212 drm-minor ", 22) ||
	    !strncmp(message, "DRMPOST 212 drm-helper ", 23) ||
	    !strncmp(message, "DRMPOST 212 drm-setup ", 22)) {
		n = a52_r228_dec(message, "n=", 0);
		if (n && n == atomic_read(&a52_r268_drm_pending)) {
			if (!strncmp(message, "DRMPOST 212 drm-acquire ", 24))
				atomic_set(&a52_r268_drm_stage, 2);
			else if (!strncmp(message, "DRMPOST 212 drm-minor ", 22))
				atomic_set(&a52_r268_drm_stage, 3);
			else if (!strncmp(message, "DRMPOST 212 drm-helper ", 23))
				atomic_set(&a52_r268_drm_stage, 4);
			else
				atomic_set(&a52_r268_drm_stage, 5);
			if (strstr(message, "rc=")) {
				rc = a52_r228_dec(message, "rc=", -61);
				atomic_set(&a52_r268_drm_rc, rc);
				if (!strncmp(message, "DRMPOST 212 drm-acquire ", 24) && rc < 0) {
					atomic_inc(&a52_r268_drm_errors);
					atomic_set(&a52_r268_drm_pending, 0);
				}
			}
		}
		return;
	}
	if (!strncmp(message, "DRMPOST 212 drm-open-ret ", 25)) {
		n = a52_r228_dec(message, "n=", 0);
		if (n && n == atomic_read(&a52_r268_drm_pending)) {
			rc = a52_r228_dec(message, "rc=", -61);
			atomic_set(&a52_r268_drm_stage, 6);
			atomic_set(&a52_r268_drm_rc, rc);
			atomic_set(&a52_r268_drm_pending, 0);
			if (rc < 0)
				atomic_inc(&a52_r268_drm_errors);
		}
		return;
	}

	if (!strncmp(message, "DRMPOST 211 open ", 17)) {
		if (a52_r268_entry_is_composer(message)) {
			n = a52_r228_dec(message, "n=", 0);
			atomic_inc(&a52_r268_msm_open_count);
			atomic_set(&a52_r268_msm_open_pending, n);
		}
		return;
	}
	if (!strncmp(message, "DRMPOST 211 open-exit ", 22)) {
		n = a52_r228_dec(message, "n=", 0);
		if (n && n == atomic_read(&a52_r268_msm_open_pending)) {
			rc = a52_r228_dec(message, "rc=", -61);
			atomic_set(&a52_r268_msm_open_rc, rc);
			atomic_set(&a52_r268_msm_open_pending, 0);
			if (rc < 0)
				atomic_inc(&a52_r268_msm_open_errors);
		}
		return;
	}

	if (!strncmp(message, "DRMPOST 211 ioctl ", 18) ||
	    !strncmp(message, "DRMPOST 211 compat ", 18)) {
		if (a52_r268_entry_is_composer(message)) {
			n = a52_r228_dec(message, "n=", 0);
			atomic_inc(&a52_r268_ioctl_count);
			atomic_set(&a52_r268_ioctl_pending, n);
			atomic_set(&a52_r268_ioctl_nr, a52_r268_hex(message, "nr=", -1));
		}
		return;
	}
	if (!strncmp(message, "DRMPOST 211 ioctl-exit ", 23) ||
	    !strncmp(message, "DRMPOST 211 compat-exit ", 23)) {
		n = a52_r228_dec(message, "n=", 0);
		if (n && n == atomic_read(&a52_r268_ioctl_pending)) {
			rc = a52_r228_dec(message, "rc=", -61);
			atomic_set(&a52_r268_ioctl_rc, rc);
			atomic_set(&a52_r268_ioctl_pending, 0);
			if (rc < 0)
				atomic_inc(&a52_r268_ioctl_errors);
		}
		return;
	}

	if (!strncmp(message, "DRMPOST 211 check ", 18)) {
		if (a52_r268_entry_is_composer(message)) {
			n = a52_r228_dec(message, "n=", 0);
			atomic_inc(&a52_r268_atomic_count);
			atomic_set(&a52_r268_atomic_pending, n);
		}
		return;
	}
	if (!strncmp(message, "DRMPOST 211 check-exit ", 23)) {
		n = a52_r228_dec(message, "n=", 0);
		if (n && n == atomic_read(&a52_r268_atomic_pending)) {
			rc = a52_r228_dec(message, "rc=", -61);
			atomic_set(&a52_r268_atomic_rc, rc);
			atomic_set(&a52_r268_atomic_pending, 0);
			if (rc < 0)
				atomic_inc(&a52_r268_atomic_errors);
		}
		return;
	}

	if (!strncmp(message, "DRMPOST 211 close ", 18) &&
	    a52_r268_entry_is_composer(message))
		atomic_inc(&a52_r268_close_count);
}

static atomic_t a52_r267_reg_stage = ATOMIC_INIT(0);
static atomic_t a52_r267_reg_rc = ATOMIC_INIT(0);
static atomic_t a52_r267_modeset = ATOMIC_INIT(-1);
static atomic_t a52_r267_plat_stage = ATOMIC_INIT(0);
static atomic_t a52_r267_plat_rc = ATOMIC_INIT(0);
static atomic_t a52_r267_dsi_done = ATOMIC_INIT(0);
static atomic_t a52_r267_smmu_done = ATOMIC_INIT(0);
static atomic_t a52_r267_probe_count = ATOMIC_INIT(0);
static atomic_t a52_r267_probe_sde = ATOMIC_INIT(-1);
static atomic_t a52_r267_probe_mdss = ATOMIC_INIT(-1);
static atomic_t a52_r267_bus_stage = ATOMIC_INIT(0);
static atomic_t a52_r267_bus_n = ATOMIC_INIT(-1);
static atomic_t a52_r267_blocks_stage = ATOMIC_INIT(0);
static atomic_t a52_r267_blocks_rc = ATOMIC_INIT(-61);
static atomic_t a52_r267_blocks_c = ATOMIC_INIT(-1);
static atomic_t a52_r267_blocks_e = ATOMIC_INIT(-1);
static atomic_t a52_r267_blocks_n = ATOMIC_INIT(-1);
static atomic_t a52_r267_blocks_p = ATOMIC_INIT(-1);
static atomic_t a52_r267_obj_stage = ATOMIC_INIT(0);
static atomic_t a52_r267_obj_rc = ATOMIC_INIT(-61);
static atomic_t a52_r267_obj_c = ATOMIC_INIT(-1);
static atomic_t a52_r267_obj_e = ATOMIC_INIT(-1);
static atomic_t a52_r267_obj_n = ATOMIC_INIT(-1);
static atomic_t a52_r267_obj_p = ATOMIC_INIT(-1);
static atomic_t a52_r267_node_count = ATOMIC_INIT(0);
static atomic_t a52_r267_node_type = ATOMIC_INIT(-1);
static atomic_t a52_r267_node_idx = ATOMIC_INIT(-1);
static atomic_t a52_r267_node_rc = ATOMIC_INIT(-61);
static atomic_t a52_r267_add_count = ATOMIC_INIT(0);
static atomic_t a52_r267_add_type = ATOMIC_INIT(-1);
static atomic_t a52_r267_add_idx = ATOMIC_INIT(-1);
static atomic_t a52_r267_add_rc = ATOMIC_INIT(-61);

static void a52_r267_track_display(const char *message)
{
	if (!message)
		return;

	if (!strncmp(message, "DISPINIT register enter", 23)) {
		atomic_set(&a52_r267_reg_stage, 1);
		atomic_set(&a52_r267_modeset,
			a52_r228_dec(message, "modeset=", atomic_read(&a52_r267_modeset)));
	} else if (!strncmp(message, "DISPINIT register disabled", 26)) {
		atomic_set(&a52_r267_reg_stage, 2);
		atomic_set(&a52_r267_reg_rc,
			a52_r228_dec(message, "rc=", atomic_read(&a52_r267_reg_rc)));
	}
	if (!strncmp(message, "DISPINIT platform-register enter", 32))
		atomic_set(&a52_r267_plat_stage, 1);
	else if (!strncmp(message, "DISPINIT platform-register exit", 31)) {
		atomic_set(&a52_r267_plat_stage, 2);
		atomic_set(&a52_r267_plat_rc,
			a52_r228_dec(message, "rc=", atomic_read(&a52_r267_plat_rc)));
	}
	if (!strncmp(message, "DISPINIT dsi-register done", 26))
		atomic_set(&a52_r267_dsi_done, 1);
	if (!strncmp(message, "DISPINIT smmu-register done", 27))
		atomic_set(&a52_r267_smmu_done, 1);
	if (!strncmp(message, "DISPINIT probe enter", 20)) {
		atomic_inc(&a52_r267_probe_count);
		atomic_set(&a52_r267_probe_sde,
			a52_r228_dec(message, "sde=", atomic_read(&a52_r267_probe_sde)));
		atomic_set(&a52_r267_probe_mdss,
			a52_r228_dec(message, "mdss=", atomic_read(&a52_r267_probe_mdss)));
	}

	if (!strncmp(message, "P267 bus-enter", 14)) {
		atomic_set(&a52_r267_bus_stage, 1);
		atomic_set(&a52_r267_bus_n,
			a52_r228_dec(message, "n=", atomic_read(&a52_r267_bus_n)));
	} else if (!strncmp(message, "P267 bus-exit", 13)) {
		atomic_set(&a52_r267_bus_stage, 2);
		atomic_set(&a52_r267_bus_n,
			a52_r228_dec(message, "n=", atomic_read(&a52_r267_bus_n)));
	} else if (!strncmp(message, "P267 blocks-enter", 17)) {
		atomic_set(&a52_r267_blocks_stage, 1);
	} else if (!strncmp(message, "P267 blocks-exit", 16)) {
		atomic_set(&a52_r267_blocks_stage, 2);
		atomic_set(&a52_r267_blocks_rc, a52_r228_dec(message, "rc=", -61));
		atomic_set(&a52_r267_blocks_c, a52_r228_dec(message, "c=", -1));
		atomic_set(&a52_r267_blocks_e, a52_r228_dec(message, "e=", -1));
		atomic_set(&a52_r267_blocks_n, a52_r228_dec(message, "n=", -1));
		atomic_set(&a52_r267_blocks_p, a52_r228_dec(message, "p=", -1));
	} else if (!strncmp(message, "P267 drm-obj-enter", 18)) {
		atomic_set(&a52_r267_obj_stage, 1);
	} else if (!strncmp(message, "P267 drm-obj-exit", 17)) {
		atomic_set(&a52_r267_obj_stage, 2);
		atomic_set(&a52_r267_obj_rc, a52_r228_dec(message, "rc=", -61));
		atomic_set(&a52_r267_obj_c, a52_r228_dec(message, "c=", -1));
		atomic_set(&a52_r267_obj_e, a52_r228_dec(message, "e=", -1));
		atomic_set(&a52_r267_obj_n, a52_r228_dec(message, "n=", -1));
		atomic_set(&a52_r267_obj_p, a52_r228_dec(message, "p=", -1));
	} else if (!strncmp(message, "P267 node-add", 13)) {
		atomic_inc(&a52_r267_add_count);
		atomic_set(&a52_r267_add_type, a52_r228_dec(message, "type=", -1));
		atomic_set(&a52_r267_add_idx, a52_r228_dec(message, "idx=", -1));
		atomic_set(&a52_r267_add_rc, a52_r228_dec(message, "rc=", -61));
	} else if (!strncmp(message, "P267 node ", 10)) {
		atomic_inc(&a52_r267_node_count);
		atomic_set(&a52_r267_node_type, a52_r228_dec(message, "type=", -1));
		atomic_set(&a52_r267_node_idx, a52_r228_dec(message, "idx=", -1));
		atomic_set(&a52_r267_node_rc, a52_r228_dec(message, "rc=", -61));
	}
}

static void a52_r228_track_message(const char *message)
{
	int stage;
	int value;

	if (!message)
		return;
	a52_r267_track_display(message);
	a52_r268_track_drm(message);
	if (!strncmp(message, "TRIPOST ", 8))
		return;

	if (a52_r228_has(message, "vold")) {
		stage = a52_r228_stage(message);
		value = a52_r228_value(message, atomic_read(&a52_r228_v_value));
		if (stage)
			atomic_set(&a52_r228_v_stage, stage);
		atomic_set(&a52_r228_v_value, value);
		atomic_inc(&a52_r228_v_count);
	}

	if (strncmp(message, "ODSPOST ", 8) == 0) {
		stage = a52_r228_stage(message);
		value = a52_r228_value(message, atomic_read(&a52_r228_o_value));
		if (a52_r228_has(message, "odsign"))
			atomic_set(&a52_r228_o_proc, 1);
		else if (a52_r228_has(message, "odrefresh"))
			atomic_set(&a52_r228_o_proc, 2);
		if (stage)
			atomic_set(&a52_r228_o_stage, stage);
		atomic_set(&a52_r228_o_value, value);
		atomic_inc(&a52_r228_o_count);
	}

	if (!strncmp(message, "BOOTPOST ", 9) &&
	    a52_r228_has(message, "surfaceflinger")) {
		stage = a52_r228_stage(message);
		value = a52_r228_value(message, atomic_read(&a52_r228_f_value));
		if (stage)
			atomic_set(&a52_r228_f_stage, stage);
		atomic_set(&a52_r228_f_value, value);
		if (stage == 1)
			atomic_inc(&a52_r228_f_count);
	}

	if (a52_r228_has(message, "GFXPOST 225 ks1")) {
		atomic_set(&a52_r228_g_open,
			a52_r228_dec(message, " o=", atomic_read(&a52_r228_g_open)));
		atomic_set(&a52_r228_g_probe,
			a52_r228_dec(message, " ps=", atomic_read(&a52_r228_g_probe)));
	}
	if (a52_r228_has(message, "GFXPOST 225 ks2")) {
		atomic_set(&a52_r228_g_reg,
			a52_r228_dec(message, " rs=", atomic_read(&a52_r228_g_reg)));
		atomic_set(&a52_r228_g_node,
			a52_r228_dec(message, " ns=", atomic_read(&a52_r228_g_node)));
	}
}

static bool a52_r267_snapshot_tick(unsigned int tick)
{
	return tick == 120U || tick == 150U || tick == 160U ||
	       tick == 170U || tick == 180U;
}

static void a52_r267_display_snapshot(unsigned int tick)
{
	if (!a52_r267_snapshot_tick(tick))
		return;

	a52_ackfr_record("P267 A t=%u rg=%d/%d ms=%d pl=%d/%d ds=%d sm=%d pr=%d/%d/%d",
		tick, atomic_read(&a52_r267_reg_stage), atomic_read(&a52_r267_reg_rc),
		atomic_read(&a52_r267_modeset), atomic_read(&a52_r267_plat_stage),
		atomic_read(&a52_r267_plat_rc), atomic_read(&a52_r267_dsi_done),
		atomic_read(&a52_r267_smmu_done), atomic_read(&a52_r267_probe_count),
		atomic_read(&a52_r267_probe_sde), atomic_read(&a52_r267_probe_mdss));
	a52_ackfr_record("P267 B t=%u bu=%d/%d bl=%d/%d ob=%d/%d nd=%d/%d/%d ad=%d/%d/%d",
		tick, atomic_read(&a52_r267_bus_stage), atomic_read(&a52_r267_bus_n),
		atomic_read(&a52_r267_blocks_stage), atomic_read(&a52_r267_blocks_rc),
		atomic_read(&a52_r267_obj_stage), atomic_read(&a52_r267_obj_rc),
		atomic_read(&a52_r267_node_count), atomic_read(&a52_r267_node_idx),
		atomic_read(&a52_r267_node_rc), atomic_read(&a52_r267_add_count),
		atomic_read(&a52_r267_add_idx), atomic_read(&a52_r267_add_rc));
	a52_ackfr_record("P267 C t=%u bc=%d,%d,%d,%d oc=%d,%d,%d,%d nt=%d at=%d",
		tick, atomic_read(&a52_r267_blocks_c), atomic_read(&a52_r267_blocks_e),
		atomic_read(&a52_r267_blocks_n), atomic_read(&a52_r267_blocks_p),
		atomic_read(&a52_r267_obj_c), atomic_read(&a52_r267_obj_e),
		atomic_read(&a52_r267_obj_n), atomic_read(&a52_r267_obj_p),
		atomic_read(&a52_r267_node_type), atomic_read(&a52_r267_add_type));
}

static void a52_r268_snapshot(unsigned int tick)
{
	if (!(tick == 120U || tick == 150U || tick == 160U ||
	      tick == 170U || tick == 180U))
		return;

	a52_ackfr_record("P268 A t=%u cp=%d ex=%d pa=%d/%d/%d dr=%d/%d/%d/%d",
		tick, atomic_read(&a52_r268_comp_pid), atomic_read(&a52_r268_exec_seen),
		atomic_read(&a52_r268_path_count), atomic_read(&a52_r268_path_kind),
		atomic_read(&a52_r268_path_fd), atomic_read(&a52_r268_drm_count),
		atomic_read(&a52_r268_drm_stage), atomic_read(&a52_r268_drm_minor),
		atomic_read(&a52_r268_drm_rc));
	a52_ackfr_record("P268 B t=%u mo=%d/%d io=%d/%d/%d ac=%d/%d cl=%d",
		tick, atomic_read(&a52_r268_msm_open_count),
		atomic_read(&a52_r268_msm_open_rc), atomic_read(&a52_r268_ioctl_count),
		atomic_read(&a52_r268_ioctl_nr), atomic_read(&a52_r268_ioctl_rc),
		atomic_read(&a52_r268_atomic_count), atomic_read(&a52_r268_atomic_rc),
		atomic_read(&a52_r268_close_count));
	a52_ackfr_record("P268 C t=%u pn=%d,%d,%d,%d,%d er=%d,%d,%d,%d,%d",
		tick, atomic_read(&a52_r268_path_pending), atomic_read(&a52_r268_drm_pending),
		atomic_read(&a52_r268_msm_open_pending), atomic_read(&a52_r268_ioctl_pending),
		atomic_read(&a52_r268_atomic_pending), atomic_read(&a52_r268_path_errors),
		atomic_read(&a52_r268_drm_errors), atomic_read(&a52_r268_msm_open_errors),
		atomic_read(&a52_r268_ioctl_errors), atomic_read(&a52_r268_atomic_errors));
}

static int a52_r228_clip(int value)
{
	if (value > 999)
		return 999;
	if (value < -999)
		return -999;
	return value;
}

static void a52_r228_tripost_snapshot(unsigned int tick)
{
	if (tick != 1U && (tick % 2U))
		return;

	a52_ackfr_record("TRIPOST 228 t=%u v=%d,%d,%d o=%d,%d,%d,%d f=%d,%d,%d g=%d,%d,%d,%d",
		tick,
		a52_r228_clip(atomic_read(&a52_r228_v_stage)),
		a52_r228_clip(atomic_read(&a52_r228_v_value)),
		a52_r228_clip(atomic_read(&a52_r228_v_count)),
		a52_r228_clip(atomic_read(&a52_r228_o_proc)),
		a52_r228_clip(atomic_read(&a52_r228_o_stage)),
		a52_r228_clip(atomic_read(&a52_r228_o_value)),
		a52_r228_clip(atomic_read(&a52_r228_o_count)),
		a52_r228_clip(atomic_read(&a52_r228_f_stage)),
		a52_r228_clip(atomic_read(&a52_r228_f_value)),
		a52_r228_clip(atomic_read(&a52_r228_f_count)),
		a52_r228_clip(atomic_read(&a52_r228_g_open)),
		a52_r228_clip(atomic_read(&a52_r228_g_probe)),
		a52_r228_clip(atomic_read(&a52_r228_g_reg)),
		a52_r228_clip(atomic_read(&a52_r228_g_node)));
}

/* A52_PHASE230_KGSL_LATE_REPLAY
 * Preserve the bounded early KGPPOST 230 journal across RAMOOPS ring
 * overwrite by replaying the original metadata records during the late
 * heartbeat window.  No match, probe, supplier, DT or return value changes.
 */
#define A52_R230_JOURNAL_CAPACITY 128U /* A52_PHASE238_CX_DRIVER_WALK_JOURNAL_V1 */
#define A52_R230_REPLAY_TICK_FIRST 150U
#define A52_R230_REPLAY_TICK_LAST 180U

static char a52_r230_journal[A52_R230_JOURNAL_CAPACITY]
	[A52_R179_MESSAGE_LEN];
static unsigned int a52_r230_journal_count;
static atomic_t a52_r230_journal_seen = ATOMIC_INIT(0);
static atomic_t a52_r230_replay_active = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(a52_r230_journal_lock);

static void a52_r230_journal_message(const char *message)
{
	unsigned long irq_flags;
	unsigned int index;

	if (!message || strncmp(message, "KGPPOST 230 ", 12) ||
	    atomic_read(&a52_r230_replay_active))
		return;

	atomic_inc(&a52_r230_journal_seen);
	spin_lock_irqsave(&a52_r230_journal_lock, irq_flags);
	index = a52_r230_journal_count;
	if (index < A52_R230_JOURNAL_CAPACITY) {
		strscpy(a52_r230_journal[index], message,
			sizeof(a52_r230_journal[index]));
		a52_r230_journal_count = index + 1;
	}
	spin_unlock_irqrestore(&a52_r230_journal_lock, irq_flags);
}

static void a52_r230_replay_journal(unsigned int tick)
{
	char message[A52_R179_MESSAGE_LEN];
	unsigned long irq_flags;
	unsigned int count;
	unsigned int index;
	unsigned int seen;

	if (tick != A52_R230_REPLAY_TICK_FIRST &&
	    tick != A52_R230_REPLAY_TICK_LAST)
		return;

	spin_lock_irqsave(&a52_r230_journal_lock, irq_flags);
	count = a52_r230_journal_count;
	spin_unlock_irqrestore(&a52_r230_journal_lock, irq_flags);
	seen = (unsigned int)atomic_read(&a52_r230_journal_seen);

	atomic_set(&a52_r230_replay_active, 1);
	a52_ackfr_record("KGPPOST 230 replay-begin t=%u kept=%u seen=%u",
		tick, count, seen);
	for (index = 0; index < count; index++) {
		spin_lock_irqsave(&a52_r230_journal_lock, irq_flags);
		strscpy(message, a52_r230_journal[index], sizeof(message));
		spin_unlock_irqrestore(&a52_r230_journal_lock, irq_flags);
		a52_ackfr_record("%s", message);
	}
	a52_ackfr_record("KGPPOST 230 replay-end t=%u kept=%u seen=%u",
		tick, count, seen);
	atomic_set(&a52_r230_replay_active, 0);
}

static void a52_r179_write_control(const char *kind)
{
	a52_ackfr_record("BOOT ctl=%s rel=%s cap=%u rs=%u copies=3 crc=crc32c",
			 kind, init_utsname()->release, A52_R179_CAPACITY,
			 A52_R179_RS_ROOTS);
}

/* A52_PHASE240_CX_FROZEN_LATCH_V1
 * Dedicated first-event latch: unlike the general KGPPOST journal, this is
 * selected only for the CX provider path and replayed independently.  Entries
 * never overwrite older entries.
 */
#define A52_R240_CXF_CAPACITY 96U
#define A52_R240_REPLAY_TICK_A 155U
#define A52_R240_REPLAY_TICK_B 170U

static char a52_r240_cxf[A52_R240_CXF_CAPACITY][A52_R179_MESSAGE_LEN];
static unsigned int a52_r240_cxf_count;
static atomic_t a52_r240_cxf_seen = ATOMIC_INIT(0);
static atomic_t a52_r240_cxf_replaying = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(a52_r240_cxf_lock);

static bool a52_r240_cxf_select(const char *message)
{
	if (!message)
		return false;
	if (!strncmp(message, "CXF240 ", 7))
		return true;
	if (!strncmp(message, "A52GDSC", 7) &&
	    (strstr(message, "CX_") || strstr(message, "gpu_cx") ||
	     strstr(message, "3d9106c")))
		return true;
	if (!strncmp(message, "G238 D", 6) && strstr(message, "3d9106c"))
		return true;
	if (!strncmp(message, "G238 P", 6) && strstr(message, "3d9106c"))
		return true;
	if (!strncmp(message, "G238 GD", 7) && strstr(message, "3d9106c"))
		return true;
	if (!strncmp(message, "KGPPOST 230 ", 12) &&
	    (strstr(message, "cxw ") || strstr(message, "3d9106c") ||
	     strstr(message, "a52-legacy-gdsc")))
		return true;
	return false;
}

static void a52_r240_cxf_latch(const char *message)
{
	unsigned long irq_flags;
	unsigned int index;

	if (atomic_read(&a52_r240_cxf_replaying) || !a52_r240_cxf_select(message))
		return;
	atomic_inc(&a52_r240_cxf_seen);
	spin_lock_irqsave(&a52_r240_cxf_lock, irq_flags);
	index = a52_r240_cxf_count;
	if (index < A52_R240_CXF_CAPACITY) {
		strscpy(a52_r240_cxf[index], message,
			sizeof(a52_r240_cxf[index]));
		a52_r240_cxf_count = index + 1;
	}
	spin_unlock_irqrestore(&a52_r240_cxf_lock, irq_flags);
}

/* A52_PHASE241_R240_REPLAY_MAYBE_UNUSED_V1: Phase 241 supersedes the heartbeat replay call. */
static void __maybe_unused a52_r240_cxf_replay(unsigned int tick)
{
	char message[A52_R179_MESSAGE_LEN];
	unsigned long irq_flags;
	unsigned int count;
	unsigned int index;
	unsigned int seen;

	if (tick != A52_R240_REPLAY_TICK_A && tick != A52_R240_REPLAY_TICK_B)
		return;

	spin_lock_irqsave(&a52_r240_cxf_lock, irq_flags);
	count = a52_r240_cxf_count;
	spin_unlock_irqrestore(&a52_r240_cxf_lock, irq_flags);
	seen = (unsigned int)atomic_read(&a52_r240_cxf_seen);

	atomic_set(&a52_r240_cxf_replaying, 1);
	a52_ackfr_record("CXF240 replay-begin t=%u kept=%u seen=%u",
			tick, count, seen);
	for (index = 0; index < count; index++) {
		spin_lock_irqsave(&a52_r240_cxf_lock, irq_flags);
		strscpy(message, a52_r240_cxf[index], sizeof(message));
		spin_unlock_irqrestore(&a52_r240_cxf_lock, irq_flags);
		a52_ackfr_record("CXF240 replay i=%u %.96s", index, message);
	}
	a52_ackfr_record("CXF240 replay-end t=%u kept=%u seen=%u",
			tick, count, seen);
	atomic_set(&a52_r240_cxf_replaying, 0);
}

/* A52_PHASE241_CX_BROAD_CORRIDOR_LATCH_V1
 * First-event retention in independent classes. Later defer/supplier spam
 * cannot overwrite early population/registration evidence.
 */
#define A52_R241_POP_CAPACITY 24U
#define A52_R241_DRV_CAPACITY 32U
#define A52_R241_PRB_CAPACITY 48U
#define A52_R241_SUP_CAPACITY 48U
#define A52_R241_REPLAY_TICK_A 155U
#define A52_R241_REPLAY_TICK_B 170U

#define A52_R241_POP 0x1U
#define A52_R241_DRV 0x2U
#define A52_R241_PRB 0x4U
#define A52_R241_SUP 0x8U

static char a52_r241_pop[A52_R241_POP_CAPACITY][A52_R179_MESSAGE_LEN];
static char a52_r241_drv[A52_R241_DRV_CAPACITY][A52_R179_MESSAGE_LEN];
static char a52_r241_prb[A52_R241_PRB_CAPACITY][A52_R179_MESSAGE_LEN];
static char a52_r241_sup[A52_R241_SUP_CAPACITY][A52_R179_MESSAGE_LEN];
static unsigned int a52_r241_pop_count;
static unsigned int a52_r241_drv_count;
static unsigned int a52_r241_prb_count;
static unsigned int a52_r241_sup_count;
static atomic_t a52_r241_pop_seen = ATOMIC_INIT(0);
static atomic_t a52_r241_drv_seen = ATOMIC_INIT(0);
static atomic_t a52_r241_prb_seen = ATOMIC_INIT(0);
static atomic_t a52_r241_sup_seen = ATOMIC_INIT(0);
static atomic_t a52_r241_replaying = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(a52_r241_pop_lock);
static DEFINE_SPINLOCK(a52_r241_drv_lock);
static DEFINE_SPINLOCK(a52_r241_prb_lock);
static DEFINE_SPINLOCK(a52_r241_sup_lock);

static bool a52_r241_gpu_context(const char *message)
{
	if (!message)
		return false;
	return strstr(message, "3d9106c") || strstr(message, "3d9100c") ||
	       strstr(message, "3d90000") || strstr(message, "3d00000") ||
	       strstr(message, "gpu_cx") || strstr(message, "gpu_gx") ||
	       strstr(message, "gpucc") || strstr(message, "kgsl") ||
	       strstr(message, "a52-legacy-gdsc") || strstr(message, "A52GDSC");
}

static unsigned int a52_r241_classify(const char *message)
{
	unsigned int mask = 0;
	bool gpu;

	/* A52_PHASE241_CXF241_SOURCE_CLASSIFICATION_V1
	 * Replay feedback is blocked by a52_r241_replaying in the latch hook.
	 * Do not suppress source-side CXF241 create-* / dreg-* evidence here.
	 */
	if (!message)
		return 0;
	gpu = a52_r241_gpu_context(message);

	if (!strncmp(message, "OFPOP ", 6) ||
	    (!strncmp(message, "G238 P", 6) && gpu) ||
	    (!strncmp(message, "P3P enter", 9) && gpu) ||
	    (!strncmp(message, "CXF241 create-", 15) && gpu))
		mask |= A52_R241_POP;

	if ((!strncmp(message, "CXF241 dreg-", 13) && gpu) ||
	    (!strncmp(message, "CXF240 drv", 10) && gpu) ||
	    (!strncmp(message, "KGPPOST 230 ", 12) &&
	     (strstr(message, "cxw ") || strstr(message, "walk-") || gpu)))
		mask |= A52_R241_DRV;

	if ((gpu && (!strncmp(message, "G238 D", 6) ||
	             !strncmp(message, "G238 P", 6) ||
	             !strncmp(message, "G238 GD", 7) ||
	             !strncmp(message, "A52GDSC", 7) ||
	             !strncmp(message, "CXF240 drv", 10))) ||
	    (!strncmp(message, "KGPPOST 230 ", 12) &&
	     (strstr(message, "cxw ") || strstr(message, "dp-") ||
	      strstr(message, "rp-") || strstr(message, "ad path=") || gpu)))
		mask |= A52_R241_PRB;

	if ((gpu && (!strncmp(message, "G238 D", 6) ||
	             !strncmp(message, "G238 GD", 7) ||
	             !strncmp(message, "A52GDSC", 7) ||
	             !strncmp(message, "CXF240 sup", 10))) ||
	    (gpu && (strstr(message, "vdd_parent") || strstr(message, "sl-") ||
	             strstr(message, "dl s=") || strstr(message, "supplier"))))
		mask |= A52_R241_SUP;
	return mask;
}

static void a52_r241_store(char (*records)[A52_R179_MESSAGE_LEN],
			   unsigned int capacity, unsigned int *count,
			   atomic_t *seen, spinlock_t *lock,
			   const char *message)
{
	unsigned long irq_flags;
	unsigned int index;

	atomic_inc(seen);
	spin_lock_irqsave(lock, irq_flags);
	index = *count;
	if (index < capacity) {
		strscpy(records[index], message, A52_R179_MESSAGE_LEN);
		*count = index + 1;
	}
	spin_unlock_irqrestore(lock, irq_flags);
}

static void a52_r241_corridor_latch(const char *message)
{
	unsigned int mask;

	if (atomic_read(&a52_r241_replaying))
		return;
	mask = a52_r241_classify(message);
	if (mask & A52_R241_POP)
		a52_r241_store(a52_r241_pop, A52_R241_POP_CAPACITY,
				&a52_r241_pop_count, &a52_r241_pop_seen,
				&a52_r241_pop_lock, message);
	if (mask & A52_R241_DRV)
		a52_r241_store(a52_r241_drv, A52_R241_DRV_CAPACITY,
				&a52_r241_drv_count, &a52_r241_drv_seen,
				&a52_r241_drv_lock, message);
	if (mask & A52_R241_PRB)
		a52_r241_store(a52_r241_prb, A52_R241_PRB_CAPACITY,
				&a52_r241_prb_count, &a52_r241_prb_seen,
				&a52_r241_prb_lock, message);
	if (mask & A52_R241_SUP)
		a52_r241_store(a52_r241_sup, A52_R241_SUP_CAPACITY,
				&a52_r241_sup_count, &a52_r241_sup_seen,
				&a52_r241_sup_lock, message);
}

static void a52_r241_replay_bucket(const char *tag,
			   char (*records)[A52_R179_MESSAGE_LEN],
			   unsigned int count, spinlock_t *lock)
{
	char message[A52_R179_MESSAGE_LEN];
	unsigned long irq_flags;
	unsigned int index;

	for (index = 0; index < count; index++) {
		spin_lock_irqsave(lock, irq_flags);
		strscpy(message, records[index], sizeof(message));
		spin_unlock_irqrestore(lock, irq_flags);
		a52_ackfr_record("CXF241 %s i=%u %.88s", tag, index, message);
	}
}

static void __maybe_unused a52_r241_corridor_replay(unsigned int tick)
{
	unsigned int pc, dc, qc, sc;
	unsigned int ps, ds, qs, ss;

	if (tick != A52_R241_REPLAY_TICK_A && tick != A52_R241_REPLAY_TICK_B)
		return;
	pc = READ_ONCE(a52_r241_pop_count);
	dc = READ_ONCE(a52_r241_drv_count);
	qc = READ_ONCE(a52_r241_prb_count);
	sc = READ_ONCE(a52_r241_sup_count);
	ps = (unsigned int)atomic_read(&a52_r241_pop_seen);
	ds = (unsigned int)atomic_read(&a52_r241_drv_seen);
	qs = (unsigned int)atomic_read(&a52_r241_prb_seen);
	ss = (unsigned int)atomic_read(&a52_r241_sup_seen);

	atomic_set(&a52_r241_replaying, 1);
	a52_ackfr_record("CXF241 replay-begin t=%u pop=%u/%u drv=%u/%u prb=%u/%u sup=%u/%u",
			 tick, pc, ps, dc, ds, qc, qs, sc, ss);
	if (tick == A52_R241_REPLAY_TICK_A) {
		a52_r241_replay_bucket("pop", a52_r241_pop, pc, &a52_r241_pop_lock);
		a52_r241_replay_bucket("drv", a52_r241_drv, dc, &a52_r241_drv_lock);
		a52_r241_replay_bucket("prb", a52_r241_prb, qc, &a52_r241_prb_lock);
		a52_r241_replay_bucket("sup", a52_r241_sup, sc, &a52_r241_sup_lock);
	}
	a52_ackfr_record("CXF241 stats t=%u pop=%u/%u drv=%u/%u prb=%u/%u sup=%u/%u",
			 tick, pc, ps, dc, ds, qc, qs, sc, ss);
	a52_ackfr_record("CXF241 replay-end t=%u", tick);
	atomic_set(&a52_r241_replaying, 0);
}

/* A52_PHASE242_CX_STICKY_STATE_V1
 * Fixed state survives the early retention hole without replaying a bucket.
 * -1 is an unseen creation state; 0/1 are observed creation outcomes;
 * -4096 is the explicit unseen return-code sentinel.
 */
#define A52_R242_UNSEEN (-4096)
static atomic_t a52_r242_cx_create = ATOMIC_INIT(-1);
static atomic_t a52_r242_gx_create = ATOMIC_INIT(-1);
static atomic_t a52_r242_dreg_seen = ATOMIC_INIT(0);
static atomic_t a52_r242_dreg_rc = ATOMIC_INIT(A52_R242_UNSEEN);
static atomic_t a52_r242_walk_seen = ATOMIC_INIT(0);
static atomic_t a52_r242_walk_rc = ATOMIC_INIT(A52_R242_UNSEEN);
static atomic_t a52_r242_match_seen = ATOMIC_INIT(0);
static atomic_t a52_r242_match_rc = ATOMIC_INIT(A52_R242_UNSEEN);
static atomic_t a52_r242_probe_seen = ATOMIC_INIT(0);
static atomic_t a52_r242_probe_rc = ATOMIC_INIT(A52_R242_UNSEEN);
static atomic_t a52_r242_sup_seen = ATOMIC_INIT(0);
static atomic_t a52_r242_sup_rc = ATOMIC_INIT(A52_R242_UNSEEN);
static atomic_t a52_r242_gdsc_seen = ATOMIC_INIT(0);
static atomic_t a52_r242_gdsc_rc = ATOMIC_INIT(A52_R242_UNSEEN);
static atomic_t a52_r242_unbound_sup_seen = ATOMIC_INIT(0);
static char a52_r242_unbound_sup[A52_R179_MESSAGE_LEN];
static DEFINE_SPINLOCK(a52_r242_unbound_sup_lock);

static void a52_r242_freeze_unbound_supplier(const char *message)
{
	unsigned long irq_flags;

	if (atomic_read(&a52_r242_unbound_sup_seen))
		return;
	spin_lock_irqsave(&a52_r242_unbound_sup_lock, irq_flags);
	if (!atomic_read(&a52_r242_unbound_sup_seen)) {
		strscpy(a52_r242_unbound_sup, message,
			sizeof(a52_r242_unbound_sup));
		atomic_set(&a52_r242_unbound_sup_seen, 1);
	}
	spin_unlock_irqrestore(&a52_r242_unbound_sup_lock, irq_flags);
}

static void __maybe_unused a52_r242_sticky_latch(const char *message)
{
	int value;

	if (!message || !strncmp(message, "CXF242 ", 7))
		return;

	if (!strncmp(message, "CXF241 create-out ", 18)) {
		value = a52_r228_dec(message, "ok=", -1);
		if (strstr(message, "3d9106c"))
			atomic_set(&a52_r242_cx_create, value);
		if (strstr(message, "3d9100c"))
			atomic_set(&a52_r242_gx_create, value);
	}
	if (!strncmp(message, "CXF241 dreg-out ", 16) &&
	    strstr(message, "a52-legacy-gdsc-regulator")) {
		atomic_inc(&a52_r242_dreg_seen);
		atomic_set(&a52_r242_dreg_rc,
			a52_r228_dec(message, "rc=", A52_R242_UNSEEN));
	}
	if (!strncmp(message, "CXF240 drvwalk-in ", 18))
		atomic_inc(&a52_r242_walk_seen);
	if (!strncmp(message, "CXF240 drvwalk-out ", 19))
		atomic_set(&a52_r242_walk_rc,
			a52_r228_dec(message, "rc=", A52_R242_UNSEEN));
	if (!strncmp(message, "CXF240 drv-match ", 17)) {
		atomic_inc(&a52_r242_match_seen);
		atomic_set(&a52_r242_match_rc,
			a52_r228_dec(message, "rc=", A52_R242_UNSEEN));
	}
	if (!strncmp(message, "CXF240 drv-probe ", 17)) {
		atomic_inc(&a52_r242_probe_seen);
		atomic_set(&a52_r242_probe_rc,
			a52_r228_dec(message, "rc=", A52_R242_UNSEEN));
	}
	if (!strncmp(message, "CXF240 sup-out ", 15)) {
		atomic_inc(&a52_r242_sup_seen);
		atomic_set(&a52_r242_sup_rc,
			a52_r228_dec(message, "rc=", A52_R242_UNSEEN));
	}
	if (!strncmp(message, "CXF240 sup n=", 13) && strstr(message, " r=-"))
		a52_r242_freeze_unbound_supplier(message);
	if (!strncmp(message, "A52GDSC CX_VDD_PARENT_GET_V1 ", 29)) {
		atomic_inc(&a52_r242_gdsc_seen);
		atomic_set(&a52_r242_gdsc_rc,
			a52_r228_dec(message, "rc=", A52_R242_UNSEEN));
	}
}

static bool a52_r242_snapshot_tick(unsigned int tick)
{
	return tick == 120 || tick == 140 || tick == 145 || tick == 150 ||
	       tick == 155 || tick == 160 || tick == 165 || tick == 170 ||
	       tick == 175;
}

static void __maybe_unused a52_r242_snapshot(unsigned int tick)
{
	char unresolved[A52_R179_MESSAGE_LEN];
	unsigned long irq_flags;

	if (!a52_r242_snapshot_tick(tick))
		return;
	a52_ackfr_record("CXF242 A t=%u c=%d g=%d dr=%d/%d dw=%d/%d dm=%d/%d",
		tick, atomic_read(&a52_r242_cx_create),
		atomic_read(&a52_r242_gx_create),
		atomic_read(&a52_r242_dreg_seen), atomic_read(&a52_r242_dreg_rc),
		atomic_read(&a52_r242_walk_seen), atomic_read(&a52_r242_walk_rc),
		atomic_read(&a52_r242_match_seen), atomic_read(&a52_r242_match_rc));
	a52_ackfr_record("CXF242 B t=%u sp=%d/%d pr=%d/%d gd=%d/%d",
		tick, atomic_read(&a52_r242_sup_seen), atomic_read(&a52_r242_sup_rc),
		atomic_read(&a52_r242_probe_seen), atomic_read(&a52_r242_probe_rc),
		atomic_read(&a52_r242_gdsc_seen), atomic_read(&a52_r242_gdsc_rc));
	if (!atomic_read(&a52_r242_unbound_sup_seen))
		return;
	spin_lock_irqsave(&a52_r242_unbound_sup_lock, irq_flags);
	strscpy(unresolved, a52_r242_unbound_sup, sizeof(unresolved));
	spin_unlock_irqrestore(&a52_r242_unbound_sup_lock, irq_flags);
	a52_ackfr_record("CXF242 U t=%u %.68s", tick, unresolved);
}


/* A52_PHASE382_STICKY_USB_CLIFF_CENSUS_V1
 *
 * Phase380/381 diagnostic prefixes were marked critical but were still
 * rejected by the older Phase243 admission gate.  Keep one small sticky USB
 * state vector and re-emit it near the 18.5 s failure frontier.  The state is
 * also copied into the raw triple-copy UFS sideband.
 */
#define A52_R382_USB_UNSEEN (-9999)
#define A52_R382_USB_PULLUP_ON_ENTER (-9998)
#define A52_R382_USB_PULLUP_OFF_ENTER (-9997)

static atomic_t a52_r382_usb_mode_in = ATOMIC_INIT(A52_R382_USB_UNSEEN);
static atomic_t a52_r382_usb_hw_mode = ATOMIC_INIT(A52_R382_USB_UNSEEN);
static atomic_t a52_r382_usb_mode_out = ATOMIC_INIT(A52_R382_USB_UNSEEN);
static atomic_t a52_r382_usb_core_mode = ATOMIC_INIT(A52_R382_USB_UNSEEN);
static atomic_t a52_r382_usb_gadget_init = ATOMIC_INIT(A52_R382_USB_UNSEEN);
static atomic_t a52_r382_usb_gadget_add_rc = ATOMIC_INIT(A52_R382_USB_UNSEEN);
static atomic_t a52_r382_usb_gs_probe_rc = ATOMIC_INIT(A52_R382_USB_UNSEEN);
static atomic_t a52_r382_usb_udc_bind_rc = ATOMIC_INIT(A52_R382_USB_UNSEEN);
static atomic_t a52_r382_usb_pullup_rc = ATOMIC_INIT(A52_R382_USB_UNSEEN);

static void a52_r385_fixed_usbdiag(unsigned int field, int value);

void a52_ackfr_usbdiag_set(unsigned int field, int value)
{
	a52_r385_fixed_usbdiag(field, value);
	switch (field) {
	case A52_ACKFR_USB_MODE_IN:
		atomic_set(&a52_r382_usb_mode_in, value);
		break;
	case A52_ACKFR_USB_HW_MODE:
		atomic_set(&a52_r382_usb_hw_mode, value);
		break;
	case A52_ACKFR_USB_MODE_OUT:
		atomic_set(&a52_r382_usb_mode_out, value);
		break;
	case A52_ACKFR_USB_CORE_MODE:
		atomic_set(&a52_r382_usb_core_mode, value);
		break;
	case A52_ACKFR_USB_GADGET_INIT:
		atomic_set(&a52_r382_usb_gadget_init, value);
		break;
	case A52_ACKFR_USB_GADGET_ADD_RC:
		atomic_set(&a52_r382_usb_gadget_add_rc, value);
		break;
	case A52_ACKFR_USB_GS_PROBE_RC:
		atomic_set(&a52_r382_usb_gs_probe_rc, value);
		break;
	case A52_ACKFR_USB_UDC_BIND_RC:
		atomic_set(&a52_r382_usb_udc_bind_rc, value);
		break;
	case A52_ACKFR_USB_PULLUP_RC:
		atomic_set(&a52_r382_usb_pullup_rc, value);
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL_GPL(a52_ackfr_usbdiag_set);

void a52_ackfr_usbdiag_snapshot(struct a52_ackfr_usbdiag_snapshot *out)
{
	if (!out)
		return;
	out->mode_in = atomic_read(&a52_r382_usb_mode_in);
	out->hw_mode = atomic_read(&a52_r382_usb_hw_mode);
	out->mode_out = atomic_read(&a52_r382_usb_mode_out);
	out->core_mode = atomic_read(&a52_r382_usb_core_mode);
	out->gadget_init = atomic_read(&a52_r382_usb_gadget_init);
	out->gadget_add_rc = atomic_read(&a52_r382_usb_gadget_add_rc);
	out->gs_probe_rc = atomic_read(&a52_r382_usb_gs_probe_rc);
	out->udc_bind_rc = atomic_read(&a52_r382_usb_udc_bind_rc);
	out->pullup_rc = atomic_read(&a52_r382_usb_pullup_rc);
}
EXPORT_SYMBOL_GPL(a52_ackfr_usbdiag_snapshot);

void a52_ackfr_usbdiag_emit(unsigned int sample)
{
	struct a52_ackfr_usbdiag_snapshot s;

	a52_ackfr_usbdiag_snapshot(&s);
	a52_ackfr_record("V382 A s=%u mi=%d hw=%d mo=%d ci=%d",
			 sample, s.mode_in, s.hw_mode, s.mode_out, s.core_mode);
	a52_ackfr_record("V382 B s=%u gi=%d ga=%d gp=%d ub=%d pr=%d",
			 sample, s.gadget_init, s.gadget_add_rc,
			 s.gs_probe_rc, s.udc_bind_rc, s.pullup_rc);
}
EXPORT_SYMBOL_GPL(a52_ackfr_usbdiag_emit);

static bool a52_r382_diag_format(const char *fmt)
{
	if (!fmt)
		return false;
	return !strncmp(fmt, "U380", 4) ||
	       !strncmp(fmt, "B380", 4) ||
	       !strncmp(fmt, "L380", 4) ||
	       !strncmp(fmt, "USB380", 6) ||
	       !strncmp(fmt, "V380", 4) ||
	       !strncmp(fmt, "V381", 4) ||
	       !strncmp(fmt, "V382", 4) ||
	       !strncmp(fmt, "P383", 4) ||
	       !strncmp(fmt, "L383", 4) ||
	       !strncmp(fmt, "U384", 4) ||
	       !strncmp(fmt, "B384", 4) ||
	       !strncmp(fmt, "L384", 4) ||
	       !strncmp(fmt, "P385", 4) ||
	       !strncmp(fmt, "B385", 4) ||
	       !strncmp(fmt, "V385", 4) || /* A52_PHASE385_FRONTIER_STALL_OBSERVER_V1 */
	       !strncmp(fmt, "P393", 4) ||
	       !strncmp(fmt, "R393", 4) ||
	       !strncmp(fmt, "M393", 4) ||
	       !strncmp(fmt, "D393", 4) ||
	       !strncmp(fmt, "I393", 4); /* A52_PHASE393_CAUSAL_CLIFF_ADMISSION_V1 */
}


/* A52_PHASE383_EVENT_FRONTIER_LOOP_AIO_V1
 *
 * Fixed boottime windows moved when diagnostics changed timing.  Anchor the
 * forensic window to the final APEX loop-device creation instead.
 */
static atomic64_t a52_r383_frontier_ns = ATOMIC64_INIT(0);
static atomic_t a52_r383_frontier_loop = ATOMIC_INIT(-1);

void a52_ackfr_frontier_trigger(unsigned int loop_no)
{
	u64 now = ktime_get_boottime_ns();

	if (atomic64_cmpxchg(&a52_r383_frontier_ns, 0, now) != 0)
		return;
	atomic_set(&a52_r383_frontier_loop, (int)loop_no);
	a52_ackfr_record("P383 trigger loop=%u t=%llu",
			 loop_no, (unsigned long long)now);
}
EXPORT_SYMBOL_GPL(a52_ackfr_frontier_trigger);

u64 a52_ackfr_frontier_trigger_ns(void)
{
	return (u64)atomic64_read(&a52_r383_frontier_ns);
}
EXPORT_SYMBOL_GPL(a52_ackfr_frontier_trigger_ns);

u64 a52_ackfr_frontier_elapsed_ms(void)
{
	u64 start = (u64)atomic64_read(&a52_r383_frontier_ns);
	u64 now;

	if (!start)
		return ~0ULL;
	now = ktime_get_boottime_ns();
	if (now < start)
		return ~0ULL;
	return div_u64(now - start, NSEC_PER_MSEC);
}
EXPORT_SYMBOL_GPL(a52_ackfr_frontier_elapsed_ms);


/* A52_PHASE385_FIXED_STICKY_V1
 *
 * The RS48 stream can wrap in roughly 230 ms under this debug load. Keep
 * Phase385 one-shot state in the otherwise-unused final 0x200 bytes of the
 * Phase380 32 KiB sideband:
 *
 *   Phase380 triple copies: 3 * 0x2a00 = 0x7e00 bytes
 *   fixed Phase385 tail:    0x7e00 .. 0x7fff = 0x200 bytes
 *
 * Two identical 256-byte copies are written on every update. Each copy is
 * CRC32C protected and contains the latest USB sticky state, UDC identity,
 * OF-simple stage, freeze observer state, UFS state and frontier heartbeat.
 * This storage is not circular.
 */
#define A52_R385_FIXED_PHYS       0xB1BFFE00ULL
#define A52_R385_FIXED_BYTES      0x200U
#define A52_R385_FIXED_COPY_BYTES 0x100U
#define A52_R385_FIXED_MAGIC      0x353833594b435453ULL
#define A52_R385_FIXED_COMMIT     0x385b0de5U
#define A52_R385_FIXED_VERSION    1U

static const char a52_r385_fixed_marker[] __used =
	"A52_PHASE385_FIXED_STICKY_V1";

struct a52_r385_fixed {
	u64 magic;
	u64 seq;
	u64 ns;
	u64 q;
	u64 freeze_age_ms;
	u64 ufs_irq_count;
	u64 ufs_outstanding;
	u32 flags;
	u32 frontier_sample;
	u32 frontier_ms;
	s32 freeze_slot;
	s32 freeze_depth;
	u32 freeze_nr;
	u32 freeze_zero;
	u32 freeze_event;
	u32 ufs_sample;
	u32 ufs_doorbell;
	u32 usb_pullup_capable;
	s32 usb_diag[9];
	char udc[24];
	char gadget[24];
	char parent[24];
	char driver[24];
	s32 ofsimple_ret;
	u32 ofsimple_stage;
	u32 crc32c;
	u32 commit;
	u32 version;
	u8 reserved[4];
};

static void *a52_r385_fixed_base;
static DEFINE_SPINLOCK(a52_r385_fixed_lock);
static struct a52_r385_fixed a52_r385_fixed_state = {
	.usb_diag = { -9999, -9999, -9999, -9999, -9999,
		      -9999, -9999, -9999, -9999 },
	.udc = "-",
	.gadget = "-",
	.parent = "-",
	.driver = "-",
};

static u32 a52_r385_fixed_crc32c(const void *buffer, size_t len)
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

static void a52_r385_fixed_commit_locked(void)
{
	void *dst0;
	void *dst1;

	if (!a52_r385_fixed_base)
		return;

	a52_r385_fixed_state.magic = A52_R385_FIXED_MAGIC;
	a52_r385_fixed_state.seq++;
	a52_r385_fixed_state.ns = ktime_get_boottime_ns();
	a52_r385_fixed_state.crc32c =
		a52_r385_fixed_crc32c(&a52_r385_fixed_state,
			offsetof(struct a52_r385_fixed, crc32c));
	a52_r385_fixed_state.commit = A52_R385_FIXED_COMMIT;
	a52_r385_fixed_state.version = A52_R385_FIXED_VERSION;

	dst0 = a52_r385_fixed_base;
	dst1 = (u8 *)a52_r385_fixed_base + A52_R385_FIXED_COPY_BYTES;
	memcpy(dst0, &a52_r385_fixed_state, sizeof(a52_r385_fixed_state));
	memcpy(dst1, &a52_r385_fixed_state, sizeof(a52_r385_fixed_state));
	wmb();
	__flush_dcache_area(dst0, sizeof(a52_r385_fixed_state));
	__flush_dcache_area(dst1, sizeof(a52_r385_fixed_state));
}

static void a52_r385_fixed_usbdiag(unsigned int field, int value)
{
	unsigned long flags;

	if (field >= ARRAY_SIZE(a52_r385_fixed_state.usb_diag))
		return;
	spin_lock_irqsave(&a52_r385_fixed_lock, flags);
	a52_r385_fixed_state.usb_diag[field] = value;
	a52_r385_fixed_state.flags |= BIT(0);
	a52_r385_fixed_commit_locked();
	spin_unlock_irqrestore(&a52_r385_fixed_lock, flags);
}

void a52_ackfr_sticky385_usb_identity(const char *udc, const char *gadget,
                                      const char *parent, const char *driver,
                                      unsigned int pullup_capable)
{
	unsigned long flags;

	spin_lock_irqsave(&a52_r385_fixed_lock, flags);
	strscpy(a52_r385_fixed_state.udc, udc ? udc : "-",
		sizeof(a52_r385_fixed_state.udc));
	strscpy(a52_r385_fixed_state.gadget, gadget ? gadget : "-",
		sizeof(a52_r385_fixed_state.gadget));
	strscpy(a52_r385_fixed_state.parent, parent ? parent : "-",
		sizeof(a52_r385_fixed_state.parent));
	strscpy(a52_r385_fixed_state.driver, driver ? driver : "-",
		sizeof(a52_r385_fixed_state.driver));
	a52_r385_fixed_state.usb_pullup_capable = pullup_capable;
	a52_r385_fixed_state.flags |= BIT(1);
	a52_r385_fixed_commit_locked();
	spin_unlock_irqrestore(&a52_r385_fixed_lock, flags);
}
EXPORT_SYMBOL_GPL(a52_ackfr_sticky385_usb_identity);

void a52_ackfr_sticky385_heartbeat(unsigned int sample, u64 elapsed_ms)
{
	unsigned long flags;

	spin_lock_irqsave(&a52_r385_fixed_lock, flags);
	a52_r385_fixed_state.frontier_sample = sample;
	a52_r385_fixed_state.frontier_ms =
		elapsed_ms > U32_MAX ? U32_MAX : (u32)elapsed_ms;
	a52_r385_fixed_state.flags |= BIT(2);
	a52_r385_fixed_commit_locked();
	spin_unlock_irqrestore(&a52_r385_fixed_lock, flags);
}
EXPORT_SYMBOL_GPL(a52_ackfr_sticky385_heartbeat);

void a52_ackfr_sticky385_freeze(unsigned int event, int slot, u64 q,
                                int depth, unsigned int nr,
                                unsigned int zero, u64 age_ms)
{
	unsigned long flags;

	spin_lock_irqsave(&a52_r385_fixed_lock, flags);
	a52_r385_fixed_state.freeze_event = event;
	a52_r385_fixed_state.freeze_slot = slot;
	a52_r385_fixed_state.q = q;
	a52_r385_fixed_state.freeze_depth = depth;
	a52_r385_fixed_state.freeze_nr = nr;
	a52_r385_fixed_state.freeze_zero = zero;
	a52_r385_fixed_state.freeze_age_ms = age_ms;
	a52_r385_fixed_state.flags |= BIT(3);
	a52_r385_fixed_commit_locked();
	spin_unlock_irqrestore(&a52_r385_fixed_lock, flags);
}
EXPORT_SYMBOL_GPL(a52_ackfr_sticky385_freeze);

void a52_ackfr_sticky385_ufs(unsigned int sample, u32 doorbell,
                             unsigned long outstanding, u64 irq_count)
{
	unsigned long flags;

	spin_lock_irqsave(&a52_r385_fixed_lock, flags);
	a52_r385_fixed_state.ufs_sample = sample;
	a52_r385_fixed_state.ufs_doorbell = doorbell;
	a52_r385_fixed_state.ufs_outstanding = (u64)outstanding;
	a52_r385_fixed_state.ufs_irq_count = irq_count;
	a52_r385_fixed_state.flags |= BIT(4);
	a52_r385_fixed_commit_locked();
	spin_unlock_irqrestore(&a52_r385_fixed_lock, flags);
}
EXPORT_SYMBOL_GPL(a52_ackfr_sticky385_ufs);

void a52_ackfr_sticky385_ofsimple(unsigned int stage, int ret)
{
	unsigned long flags;

	spin_lock_irqsave(&a52_r385_fixed_lock, flags);
	a52_r385_fixed_state.ofsimple_stage = stage;
	a52_r385_fixed_state.ofsimple_ret = ret;
	a52_r385_fixed_state.flags |= BIT(5);
	a52_r385_fixed_commit_locked();
	spin_unlock_irqrestore(&a52_r385_fixed_lock, flags);
}
EXPORT_SYMBOL_GPL(a52_ackfr_sticky385_ofsimple);

static int __init __maybe_unused a52_r385_fixed_init(void)
{
	BUILD_BUG_ON(sizeof(struct a52_r385_fixed) != A52_R385_FIXED_COPY_BYTES);
	a52_r385_fixed_base = memremap(A52_R385_FIXED_PHYS,
		A52_R385_FIXED_BYTES, MEMREMAP_WB);
	if (!a52_r385_fixed_base)
		return 0;

	memset(a52_r385_fixed_base, 0, A52_R385_FIXED_BYTES);
	wmb();
	__flush_dcache_area(a52_r385_fixed_base, A52_R385_FIXED_BYTES);
	a52_r385_fixed_commit_locked();
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired core_initcall(a52_r385_fixed_init); */


/* A52_PHASE392_PERSISTENT_GAP_DUAL_BACKEND_V1
 *
 * Phase391 proved Samsung persistent gap is rewritten by XBL before recovery.
 * Samsung's preserved downstream boot log proves sec_debug_region@B1000000
 * is exactly 9 MiB, ending at B18FFFFF. Recovery /proc/iomem proves normal
 * System RAM resumes at B1B00000. Therefore B1900000..B1AFFFFF is a 2 MiB
 * window outside both Samsung sec_debug ownership and the Linux page allocator.
 *
 * The Phase390 recovery snapshot also retained stale historical bytes in this
 * 2 MiB window after reboot, strong evidence XBL does not overwrite it.
 *
 * Existing 1 MiB ramoops/R48 remains unchanged. This backend stores fixed
 * 128-byte CRC32C records and is recovered from the full 11 MiB Phase390
 * snapshot. No UFS/block I/O occurs in the recorder hot path.
 *
 * 4 KiB header + 16,352 x 128-byte records = exactly 2 MiB.
 */
#define A52_P392_PHYS		0xB1900000ULL
#define A52_P392_BYTES		SZ_1M /* Phase414 leaves B1A00000 free */
#define A52_P392_HEADER_BYTES	SZ_4K
#define A52_P392_RECORD_BYTES	128U
#define A52_P392_COPIES	3U
#define A52_P392_CAPACITY	((A52_P392_BYTES - A52_P392_HEADER_BYTES) / (A52_P392_RECORD_BYTES * A52_P392_COPIES))
#define A52_P392_COPY_BYTES	(A52_P392_CAPACITY * A52_P392_RECORD_BYTES)
#define A52_P392_MAGIC		0x323933474f4c5047ULL /* GPLOG392 */
#define A52_P392_REC_COMMIT	0x392c0de5U
#define A52_P392_VERSION	2U

struct a52_p392_header {
	u64 magic;
	u32 version;
	u32 record_bytes;
	u32 capacity;
	u32 reserved0;
	u64 boot_id;
	u64 write_seq;
	u64 write_seq_inv;
	u64 dropped;
	u64 dropped_inv;
};

struct a52_p392_record {
	u64 seq;
	u64 ts_ns;
	u16 cpu;
	u16 len;
	u32 reserved0;
	char text[96];
	u32 crc32c;
	u32 commit;
} __packed;

static const char a52_p392_marker[] __used =
	"A52_PHASE392_PERSISTENT_GAP_DUAL_BACKEND_V1";
static void __iomem *a52_p392_base;
static DEFINE_SPINLOCK(a52_p392_lock);
static u64 a52_p392_seq;
static u64 a52_p392_boot_id;
static atomic64_t a52_p392_dropped = ATOMIC64_INIT(0);

static u32 a52_p392_crc32c(const void *buffer, size_t len)
{
	const u8 *bytes = buffer;
	u32 crc = ~0U;
	size_t index;
	unsigned int bit;

	for (index = 0; index < len; index++) {
		crc ^= bytes[index];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
				((crc & 1U) ? 0x82f63b78U : 0U);
	}
	return ~crc;
}

/* A52_PHASE405_DMA_DONE_DIRECT_V1
 *
 * Fixed, two-copy DSI DMA_DONE flight recorder in the Phase392 metadata page.
 * Phase394 owns 0x200..0x4ff; Phase405 starts exactly at 0x500.
 *
 * copy0: 0x500..0xa7f = 11 x 128 bytes
 * copy1: 0xa80..0xfff = 11 x 128 bytes
 *
 * Each stage has a dedicated slot, so later traffic cannot overwrite an
 * earlier stage.  Only already-formatted P276 303/387 records are mirrored.
 */
#define A52_P405_OFF0          0x500U
#define A52_P405_OFF1          0xa80U
#define A52_P405_SLOT_BYTES    128U
#define A52_P405_SLOTS         11U
#define A52_P405_MAGIC         0x3530344953445841ULL
#define A52_P405_COMMIT        0x405c0de5U

struct a52_p405_slot {
	u64 magic;
	u64 ns;
	u32 seq;
	u32 stage;
	u16 cpu;
	u16 len;
	u32 reserved0;
	char text[88];
	u32 crc32c;
	u32 commit;
} __packed;

static atomic_t a52_p405_seq = ATOMIC_INIT(0);

static void a52_p406_persist(void __iomem *addr, size_t len)
{
	if (!addr || !len)
		return;
	wmb();
	__flush_dcache_area((void __force *)addr, len);
	wmb();
}


static int a52_p405_stage(const char *message)
{
	if (!message)
		return -1;
	if (!strncmp(message, "P276 303 S00 ", 13))
		return 0;
	if (!strncmp(message, "P276 303 S03 ", 13))
		return 1;
	if (!strncmp(message, "P276 303 S04 ", 13))
		return 2;
	if (!strncmp(message, "P276 303 S05 ", 13))
		return 3;
	if (!strncmp(message, "P276 303 S06 ", 13))
		return 4;
	if (!strncmp(message, "P276 387D e ", 12))
		return 5;
	if (!strncmp(message, "P276 387I ", 10))
		return 6;
	if (!strncmp(message, "P276 387D w ", 12))
		return 7;
	if (!strncmp(message, "P276 387D f ", 12))
		return 8;
	if (!strncmp(message, "P276 303 S08 ", 13))
		return 9;
	if (!strncmp(message, "P276 303 S09 ", 13))
		return 10;
	return -1;
}

static void __maybe_unused a52_p405_dsi_mirror(const char *message)
{
	struct a52_p405_slot slot;
	void __iomem *base;
	size_t len;
	int stage;

	BUILD_BUG_ON(sizeof(struct a52_p405_slot) != A52_P405_SLOT_BYTES);
	BUILD_BUG_ON(A52_P405_OFF1 +
		     A52_P405_SLOTS * A52_P405_SLOT_BYTES >
		     A52_P392_HEADER_BYTES);

	stage = a52_p405_stage(message);
	if (stage < 0)
		return;
	base = READ_ONCE(a52_p392_base);
	if (!base)
		return;

	memset(&slot, 0, sizeof(slot));
	slot.magic = A52_P405_MAGIC;
	slot.ns = ktime_get_boottime_ns();
	slot.seq = (u32)atomic_inc_return(&a52_p405_seq);
	slot.stage = (u32)stage;
	slot.cpu = (u16)raw_smp_processor_id();
	len = strnlen(message, sizeof(slot.text) - 1U);
	slot.len = (u16)len;
	memcpy(slot.text, message, len);
	slot.text[len] = '\0';
	slot.crc32c = a52_p392_crc32c(&slot,
		offsetof(struct a52_p405_slot, crc32c));
	slot.commit = A52_P405_COMMIT;

	{
		void __iomem *dst0 = (u8 __iomem *)base + A52_P405_OFF0 +
			(unsigned int)stage * A52_P405_SLOT_BYTES;
		void __iomem *dst1 = (u8 __iomem *)base + A52_P405_OFF1 +
			(unsigned int)stage * A52_P405_SLOT_BYTES;

		memcpy_toio(dst0, &slot, sizeof(slot));
		memcpy_toio(dst1, &slot, sizeof(slot));
		a52_p406_persist(dst0, sizeof(slot));
		a52_p406_persist(dst1, sizeof(slot));
	}
}



static void a52_p392_header_pair(size_t off, u64 value)
{
	u64 pair[2] = { value, ~value };
	void __iomem *dst = (u8 __iomem *)a52_p392_base + off;

	memcpy_toio(dst, pair, sizeof(pair));
	a52_p406_persist(dst, sizeof(pair));
}

static void a52_p392_gap_write(const char *message)
{
	struct a52_p392_record rec;
	unsigned long flags;
	u64 slot;
	u64 dropped;
	size_t len;
	unsigned int copy;

	BUILD_BUG_ON(sizeof(struct a52_p392_record) != A52_P392_RECORD_BYTES);

	if (!READ_ONCE(a52_p392_base) || !message)
		return;

	if (!spin_trylock_irqsave(&a52_p392_lock, flags)) {
		atomic64_inc(&a52_p392_dropped);
		return;
	}

	memset(&rec, 0, sizeof(rec));
	rec.seq = ++a52_p392_seq;
	rec.ts_ns = ktime_get_boottime_ns();
	rec.cpu = (u16)raw_smp_processor_id();
	len = strnlen(message, sizeof(rec.text) - 1U);
	rec.len = (u16)len;
	memcpy(rec.text, message, len);
	rec.text[len] = '\0';
	rec.crc32c = a52_p392_crc32c(&rec,
				     offsetof(struct a52_p392_record, crc32c));
	rec.commit = A52_P392_REC_COMMIT;

	slot = (rec.seq - 1ULL) % A52_P392_CAPACITY;
	for (copy = 0; copy < A52_P392_COPIES; copy++) {
		void __iomem *dst =
			(u8 __iomem *)a52_p392_base + A52_P392_HEADER_BYTES +
			copy * A52_P392_COPY_BYTES +
			slot * A52_P392_RECORD_BYTES;

		memcpy_toio(dst, &rec, sizeof(rec));
		a52_p406_persist(dst, sizeof(rec));
	}

	a52_p392_header_pair(offsetof(struct a52_p392_header, write_seq),
			     a52_p392_seq);
	dropped = (u64)atomic64_read(&a52_p392_dropped);
	a52_p392_header_pair(offsetof(struct a52_p392_header, dropped),
			     dropped);
	spin_unlock_irqrestore(&a52_p392_lock, flags);
}

static int __init a52_p392_init(void)
{
	struct a52_p392_header old = { 0 };
	struct a52_p392_header h = { 0 };

	BUILD_BUG_ON(sizeof(struct a52_p392_header) > A52_P392_HEADER_BYTES);
	BUILD_BUG_ON(A52_P392_HEADER_BYTES +
		     A52_P392_COPIES * A52_P392_COPY_BYTES >
		     A52_P392_BYTES);

	a52_p392_base = ioremap_cache(A52_P392_PHYS, A52_P392_BYTES);
	if (!a52_p392_base) {
		a52_ackfr_record("P392 PERSISTENT_GAP map=0");
		return 0;
	}

	memcpy_fromio(&old, a52_p392_base, sizeof(old));
	if (old.magic == A52_P392_MAGIC &&
	    old.version == A52_P392_VERSION &&
	    old.boot_id && old.boot_id != ~0ULL)
		a52_p392_boot_id = old.boot_id + 1ULL;
	else
		a52_p392_boot_id = 1ULL;

	h.magic = A52_P392_MAGIC;
	h.version = A52_P392_VERSION;
	h.record_bytes = A52_P392_RECORD_BYTES;
	h.capacity = A52_P392_CAPACITY;
	h.boot_id = a52_p392_boot_id;
	h.write_seq_inv = ~0ULL;
	h.dropped_inv = ~0ULL;

	/* Only reset metadata. Old slots are ignored until write_seq reaches them. */
	memset_io(a52_p392_base, 0, A52_P392_HEADER_BYTES);
	memcpy_toio(a52_p392_base, &h, sizeof(h));
	a52_p406_persist(a52_p392_base, A52_P392_HEADER_BYTES);

	a52_p392_seq = 0;
	atomic64_set(&a52_p392_dropped, 0);
	a52_ackfr_record("P392 PERSISTENT_GAP map=1 phys=%llx bytes=%x cap=%u boot=%llu",
		(unsigned long long)A52_P392_PHYS, A52_P392_BYTES,
		A52_P392_CAPACITY, (unsigned long long)a52_p392_boot_id);
	return 0;
}
core_initcall_sync(a52_p392_init);


/* A52_PHASE414_SEQUENTIAL_3M_RECORDER_V1
 *
 * One non-wrapping chronological recorder:
 *
 *   records 1 .. N1  -> Samsung debug partition +0x800000, 2 MiB
 *   records N1+1..N2 -> persistent reserved RAM 0xB1A00000, 1 MiB
 *   after N2          -> stop, never overwrite earlier evidence
 *
 * The Samsung region is the logical first tier. A 2 MiB vmalloc staging image
 * exists only so hot-path callers never wait for block I/O. A workqueue writes
 * completed pages and a bounce-copy of the current partial page in background.
 *
 * R48/RS48 remains completely separate and unchanged.
 *
 * No 2 MiB CRC is used. Persistence/identity use magic, phase, boot_id,
 * monotonic global sequence, record count, and commit words.
 */
#define A52_P414_DEBUG_OFFSET        0x00800000ULL
#define A52_P414_DISK_BYTES          (2U * SZ_1M)
#define A52_P414_RAM_PHYS            0xB1A00000ULL
#define A52_P414_RAM_BYTES           0x00062000U /* A52_PHASE444_RESERVED_PARTITION_V2 */
#define A52_P414_HEADER_BYTES        SZ_4K
#define A52_P414_RECORD_BYTES        128U
#define A52_P414_DISK_CAPACITY       ((A52_P414_DISK_BYTES - A52_P414_HEADER_BYTES) / A52_P414_RECORD_BYTES)
#define A52_P414_RAM_CAPACITY        ((A52_P414_RAM_BYTES - A52_P414_HEADER_BYTES) / A52_P414_RECORD_BYTES)
#define A52_P414_MAGIC               0x3431344d33434553ULL
#define A52_P414_VERSION             1U
#define A52_P414_REC_COMMIT          0x414c0de5U
#define A52_P414_HDR_COMMIT          0x414b0071U
#define A52_P414_STATE_DISK          1U
#define A52_P414_STATE_RAM           2U
#define A52_P414_STATE_FULL          3U

struct a52_p414_header {
	u64 magic;
	u32 version;
	u32 phase;
	u64 boot_id;
	u64 global_seq;
	u32 disk_count;
	u32 ram_count;
	u32 disk_capacity;
	u32 ram_capacity;
	u32 record_bytes;
	u32 state;
	u32 dropped;
	u32 commit;
} __packed;

struct a52_p414_record {
	u64 seq;
	u64 ts_ns;
	u64 boot_id;
	u32 phase;
	u16 cpu;
	u16 len;
	char text[88];
	u32 commit;
	u32 reserved;
} __packed;

static DEFINE_SPINLOCK(a52_p414_lock);
static u8 *a52_p414_disk_stage;
static u8 *a52_p414_bounce;
static void __iomem *a52_p414_ram;
static u64 a52_p414_boot_id;
static u64 a52_p414_seq;
static u32 a52_p414_disk_count;
static u32 a52_p414_ram_count;
static u32 a52_p414_state;
static u32 a52_p414_dropped;
static u32 a52_p414_flushed_full_pages;
static atomic_t a52_p414_ready = ATOMIC_INIT(0);
static atomic_t a52_p414_disk_gen = ATOMIC_INIT(0);
static atomic_t a52_p414_disk_written_gen = ATOMIC_INIT(0);
static atomic_t a52_p414_disk_retry = ATOMIC_INIT(0);

/* A52_PHASE416_SEQUENTIAL_3M_DISK_PERSIST_V2
 *
 * Phase414 correctly staged the chronological 2 MiB disk tier, but Phase415
 * recovery proved that the +0x800000 Samsung image remained stale Phase413.
 * Keep the Phase414 record format/capacity unchanged and harden only transport:
 *
 *  - core_initcall still allocates/stages the earliest records;
 *  - raw block I/O is not armed until late_initcall, after device init;
 *  - pre-late records remain in the 2 MiB vmalloc staging image;
 *  - late arm flushes the current generation, then normal append events nudge it;
 *  - B1A header is updated during the disk tier too, so logical count survives;
 *  - three redundant 128-byte status copies in B1A expose open/BIO/flush rc,
 *    retries, generations, and last page even when Samsung persistence fails.
 */
#define A52_P416_STATUS_MAGIC         0x3631345354415433ULL /* 3TATS416 */
#define A52_P416_STATUS_VERSION       1U
#define A52_P416_STATUS_COMMIT        0x416c0de5U
#define A52_P416_STATUS_OFFSET0       0x00000100U
#define A52_P416_STATUS_OFFSET1       0x00000180U
#define A52_P416_STATUS_OFFSET2       0x00000200U

struct a52_p416_status {
	u64 magic;
	u64 magic_inv;
	u32 version;
	u32 version_inv;
	u32 armed;
	u32 armed_inv;
	u32 worker_runs;
	u32 worker_runs_inv;
	s32 open_rc;
	u32 open_rc_inv;
	s32 submit_rc;
	u32 submit_rc_inv;
	s32 flush_rc;
	u32 flush_rc_inv;
	u32 retry_count;
	u32 retry_count_inv;
	u32 disk_gen;
	u32 disk_gen_inv;
	u32 written_gen;
	u32 written_gen_inv;
	u32 disk_count;
	u32 disk_count_inv;
	u32 last_page;
	u32 last_page_inv;
	u32 state;
	u32 state_inv;
	u32 commit;
	u32 commit_inv;
	u32 reserved[2];
} __packed;

static u32 a52_p416_disk_armed;
static u32 a52_p416_worker_runs;
static s32 a52_p416_open_rc = -EAGAIN;
static s32 a52_p416_submit_rc = -EAGAIN;
static s32 a52_p416_flush_rc = -EAGAIN;
static u32 a52_p416_last_page;

static void a52_p416_fill_status(struct a52_p416_status *s)
{
	u32 open_bits = (u32)a52_p416_open_rc;
	u32 submit_bits = (u32)a52_p416_submit_rc;
	u32 flush_bits = (u32)a52_p416_flush_rc;
	u32 retries = (u32)atomic_read(&a52_p414_disk_retry);
	u32 gen = (u32)atomic_read(&a52_p414_disk_gen);
	u32 written = (u32)atomic_read(&a52_p414_disk_written_gen);

	memset(s, 0, sizeof(*s));
	s->magic = A52_P416_STATUS_MAGIC;
	s->magic_inv = ~A52_P416_STATUS_MAGIC;
	s->version = A52_P416_STATUS_VERSION;
	s->version_inv = ~A52_P416_STATUS_VERSION;
	s->armed = a52_p416_disk_armed;
	s->armed_inv = ~s->armed;
	s->worker_runs = a52_p416_worker_runs;
	s->worker_runs_inv = ~s->worker_runs;
	s->open_rc = a52_p416_open_rc;
	s->open_rc_inv = ~open_bits;
	s->submit_rc = a52_p416_submit_rc;
	s->submit_rc_inv = ~submit_bits;
	s->flush_rc = a52_p416_flush_rc;
	s->flush_rc_inv = ~flush_bits;
	s->retry_count = retries;
	s->retry_count_inv = ~retries;
	s->disk_gen = gen;
	s->disk_gen_inv = ~gen;
	s->written_gen = written;
	s->written_gen_inv = ~written;
	s->disk_count = a52_p414_disk_count;
	s->disk_count_inv = ~s->disk_count;
	s->last_page = a52_p416_last_page;
	s->last_page_inv = ~s->last_page;
	s->state = a52_p414_state;
	s->state_inv = ~s->state;
	s->commit = A52_P416_STATUS_COMMIT;
	s->commit_inv = ~A52_P416_STATUS_COMMIT;
}

static void a52_p416_sync_status_locked(void)
{
	struct a52_p416_status s;
	const u32 offsets[] = {
		A52_P416_STATUS_OFFSET0,
		A52_P416_STATUS_OFFSET1,
		A52_P416_STATUS_OFFSET2,
	};
	unsigned int i;

	if (!a52_p414_ram)
		return;

	a52_p416_fill_status(&s);
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		if (a52_p414_disk_stage)
			memcpy(a52_p414_disk_stage + offsets[i], &s, sizeof(s));
		memcpy_toio((u8 __iomem *)a52_p414_ram + offsets[i],
			    &s, sizeof(s));
	}
	a52_p406_persist((u8 __iomem *)a52_p414_ram + A52_P416_STATUS_OFFSET0,
			 A52_P416_STATUS_OFFSET2 - A52_P416_STATUS_OFFSET0 +
			 sizeof(s));
}

static void a52_p414_fill_header(struct a52_p414_header *h)
{
	memset(h, 0, sizeof(*h));
	h->magic = A52_P414_MAGIC;
	h->version = A52_P414_VERSION;
	h->phase = 414U;
	h->boot_id = a52_p414_boot_id;
	h->global_seq = a52_p414_seq;
	h->disk_count = a52_p414_disk_count;
	h->ram_count = a52_p414_ram_count;
	h->disk_capacity = A52_P414_DISK_CAPACITY;
	h->ram_capacity = A52_P414_RAM_CAPACITY;
	h->record_bytes = A52_P414_RECORD_BYTES;
	h->state = a52_p414_state;
	h->dropped = a52_p414_dropped;
	h->commit = A52_P414_HDR_COMMIT;
}

static void a52_p414_update_disk_header_locked(void)
{
	struct a52_p414_header h;

	if (!a52_p414_disk_stage)
		return;
	a52_p414_fill_header(&h);
	memcpy(a52_p414_disk_stage, &h, sizeof(h));
	wmb();
}

static void a52_p414_update_ram_header_locked(void)
{
	struct a52_p414_header h;

	if (!a52_p414_ram)
		return;
	a52_p414_fill_header(&h);
	memcpy_toio(a52_p414_ram, &h, sizeof(h));
	a52_p406_persist(a52_p414_ram, sizeof(h));
}

static int a52_p414_submit_page(struct block_device *bdev, void *addr,
				unsigned int page_index)
{
	struct bio *bio;
	struct page *page;
	int added;
	int rc;

	if (!bdev || !addr)
		return -EINVAL;

	page = vmalloc_to_page(addr);
	if (!page)
		return -EFAULT;

	bio = bio_alloc(GFP_KERNEL, 1);
	if (!bio)
		return -ENOMEM;

	bio_set_dev(bio, bdev);
	bio->bi_iter.bi_sector =
		(sector_t)((A52_P414_DEBUG_OFFSET +
			    (u64)page_index * PAGE_SIZE) >> 9);
	bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_SYNC | REQ_FUA);
	added = bio_add_page(bio, page, PAGE_SIZE, 0);
	if (added != PAGE_SIZE) {
		bio_put(bio);
		return -EIO;
	}

	rc = submit_bio_wait(bio);
	bio_put(bio);
	return rc;
}

static void a52_p414_disk_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(a52_p414_disk_work, a52_p414_disk_workfn);

static void a52_p414_queue_disk(void)
{
	atomic_inc(&a52_p414_disk_gen);
	if (READ_ONCE(a52_p416_disk_armed))
		mod_delayed_work(system_unbound_wq, &a52_p414_disk_work, 0);
}

static void a52_p414_disk_workfn(struct work_struct *work)
{
	struct block_device *bdev;
	dev_t devt = MKDEV(SCSI_DISK0_MAJOR, 8);
	unsigned long flags;
	unsigned int count;
	unsigned int full_pages;
	unsigned int partial_bytes;
	unsigned int page_index;
	unsigned int generation;
	int rc = 0;
	int retry;

	(void)work;
	if (!READ_ONCE(a52_p414_disk_stage) || !READ_ONCE(a52_p414_bounce))
		return;

	generation = (unsigned int)atomic_read(&a52_p414_disk_gen);
	spin_lock_irqsave(&a52_p414_lock, flags);
	a52_p416_worker_runs++;
	a52_p416_open_rc = -EINPROGRESS;
	a52_p416_submit_rc = -EAGAIN;
	a52_p416_flush_rc = -EAGAIN;
	a52_p416_sync_status_locked();
	spin_unlock_irqrestore(&a52_p414_lock, flags);

	bdev = blkdev_get_by_dev(devt, FMODE_WRITE, NULL);
	if (IS_ERR(bdev)) {
		int open_rc = PTR_ERR(bdev);

		retry = atomic_inc_return(&a52_p414_disk_retry);
		spin_lock_irqsave(&a52_p414_lock, flags);
		a52_p416_open_rc = open_rc;
		a52_p416_sync_status_locked();
		spin_unlock_irqrestore(&a52_p414_lock, flags);
		if (retry <= 240)
			mod_delayed_work(system_unbound_wq, &a52_p414_disk_work,
					 msecs_to_jiffies(250));
		return;
	}
	spin_lock_irqsave(&a52_p414_lock, flags);
	a52_p416_open_rc = 0;
	a52_p416_sync_status_locked();
	spin_unlock_irqrestore(&a52_p414_lock, flags);

	/* Header is mutable, so always write it from a locked bounce snapshot. */
	spin_lock_irqsave(&a52_p414_lock, flags);
	memcpy(a52_p414_bounce, a52_p414_disk_stage, PAGE_SIZE);
	count = a52_p414_disk_count;
	spin_unlock_irqrestore(&a52_p414_lock, flags);

	a52_p416_last_page = 0U;
	rc = a52_p414_submit_page(bdev, a52_p414_bounce, 0U);
	spin_lock_irqsave(&a52_p414_lock, flags);
	a52_p416_submit_rc = rc;
	a52_p416_sync_status_locked();
	spin_unlock_irqrestore(&a52_p414_lock, flags);
	if (rc)
		goto out;

	full_pages = (count * A52_P414_RECORD_BYTES) / PAGE_SIZE;
	partial_bytes = (count * A52_P414_RECORD_BYTES) % PAGE_SIZE;

	/* Completed data pages are immutable and can be submitted directly. */
	for (page_index = a52_p414_flushed_full_pages + 1U;
	     page_index <= full_pages; page_index++) {
		a52_p416_last_page = page_index;
		rc = a52_p414_submit_page(
			bdev, a52_p414_disk_stage + page_index * PAGE_SIZE,
			page_index);
		spin_lock_irqsave(&a52_p414_lock, flags);
		a52_p416_submit_rc = rc;
		a52_p416_sync_status_locked();
		spin_unlock_irqrestore(&a52_p414_lock, flags);
		if (rc)
			goto out;
		a52_p414_flushed_full_pages = page_index;
	}

	/* Preserve the current partial page too, without racing hot-path writes. */
	if (partial_bytes) {
		page_index = 1U + full_pages;
		spin_lock_irqsave(&a52_p414_lock, flags);
		memcpy(a52_p414_bounce,
		       a52_p414_disk_stage + page_index * PAGE_SIZE,
		       PAGE_SIZE);
		spin_unlock_irqrestore(&a52_p414_lock, flags);
		a52_p416_last_page = page_index;
		rc = a52_p414_submit_page(bdev, a52_p414_bounce, page_index);
		spin_lock_irqsave(&a52_p414_lock, flags);
		a52_p416_submit_rc = rc;
		a52_p416_sync_status_locked();
		spin_unlock_irqrestore(&a52_p414_lock, flags);
		if (rc)
			goto out;
	}

	rc = blkdev_issue_flush(bdev, GFP_KERNEL);
	spin_lock_irqsave(&a52_p414_lock, flags);
	a52_p416_flush_rc = rc;
	a52_p416_sync_status_locked();
	spin_unlock_irqrestore(&a52_p414_lock, flags);
out:
	blkdev_put(bdev, FMODE_WRITE);

	if (!rc) {
		atomic_set(&a52_p414_disk_written_gen, generation);
		atomic_set(&a52_p414_disk_retry, 0);
		spin_lock_irqsave(&a52_p414_lock, flags);
		a52_p416_sync_status_locked();
		spin_unlock_irqrestore(&a52_p414_lock, flags);
		if ((unsigned int)atomic_read(&a52_p414_disk_gen) != generation)
			mod_delayed_work(system_unbound_wq,
				&a52_p414_disk_work, 0);
		return;
	}

	retry = atomic_inc_return(&a52_p414_disk_retry);
	spin_lock_irqsave(&a52_p414_lock, flags);
	a52_p416_sync_status_locked();
	spin_unlock_irqrestore(&a52_p414_lock, flags);
	if (retry <= 240)
		mod_delayed_work(system_unbound_wq, &a52_p414_disk_work,
				 msecs_to_jiffies(250));
}

static void a52_p414_append_text(const char *message)
{
	struct a52_p414_record rec;
	unsigned long flags;
	size_t len;
	bool queue_disk = false;

	if (!message || !atomic_read(&a52_p414_ready))
		return;

	if (!spin_trylock_irqsave(&a52_p414_lock, flags)) {
		a52_p414_dropped++;
		return;
	}

	memset(&rec, 0, sizeof(rec));
	rec.seq = ++a52_p414_seq;
	rec.ts_ns = ktime_get_boottime_ns();
	rec.boot_id = a52_p414_boot_id;
	rec.phase = 414U;
	rec.cpu = (u16)raw_smp_processor_id();
	len = strnlen(message, sizeof(rec.text) - 1U);
	rec.len = (u16)len;
	memcpy(rec.text, message, len);
	rec.text[len] = '\0';
	rec.commit = A52_P414_REC_COMMIT;

	if (a52_p414_disk_count < A52_P414_DISK_CAPACITY) {
		memcpy(a52_p414_disk_stage + A52_P414_HEADER_BYTES +
		       a52_p414_disk_count * A52_P414_RECORD_BYTES,
		       &rec, sizeof(rec));
		a52_p414_disk_count++;
		if (a52_p414_disk_count == A52_P414_DISK_CAPACITY) {
			a52_p414_state = A52_P414_STATE_RAM;
			a52_p414_update_ram_header_locked();
		}
		a52_p414_update_disk_header_locked();
		a52_p414_update_ram_header_locked();
		a52_p416_sync_status_locked();
		queue_disk = true;
	} else if (a52_p414_ram_count < A52_P414_RAM_CAPACITY) {
		void __iomem *dst =
			(u8 __iomem *)a52_p414_ram + A52_P414_HEADER_BYTES +
			a52_p414_ram_count * A52_P414_RECORD_BYTES;

		memcpy_toio(dst, &rec, sizeof(rec));
		a52_p406_persist(dst, sizeof(rec));
		a52_p414_ram_count++;
		if (a52_p414_ram_count == A52_P414_RAM_CAPACITY)
			a52_p414_state = A52_P414_STATE_FULL;
		a52_p414_update_ram_header_locked();
	} else {
		a52_p414_dropped++;
		a52_p414_state = A52_P414_STATE_FULL;
		a52_p414_update_ram_header_locked();
	}

	spin_unlock_irqrestore(&a52_p414_lock, flags);

	if (queue_disk)
		a52_p414_queue_disk();
}

static int __init a52_p414_init(void)
{
	struct a52_p414_header old = { 0 };
	struct a52_p414_header h;

	BUILD_BUG_ON(sizeof(struct a52_p414_header) != 64U);
	BUILD_BUG_ON(sizeof(struct a52_p416_status) != 128U);
	BUILD_BUG_ON(sizeof(struct a52_p414_record) != A52_P414_RECORD_BYTES);
	BUILD_BUG_ON(A52_P414_HEADER_BYTES +
		     A52_P414_DISK_CAPACITY * A52_P414_RECORD_BYTES !=
		     A52_P414_DISK_BYTES);
	BUILD_BUG_ON(A52_P414_HEADER_BYTES +
		     A52_P414_RAM_CAPACITY * A52_P414_RECORD_BYTES !=
		     A52_P414_RAM_BYTES);
	BUILD_BUG_ON(PAGE_SIZE != 4096);

	a52_p414_disk_stage = vzalloc(A52_P414_DISK_BYTES);
	a52_p414_bounce = vzalloc(PAGE_SIZE);
	a52_p414_ram = ioremap_cache(A52_P414_RAM_PHYS, A52_P414_RAM_BYTES);
	if (!a52_p414_disk_stage || !a52_p414_bounce || !a52_p414_ram)
		return 0;

	memcpy_fromio(&old, a52_p414_ram, sizeof(old));
	if (old.magic == A52_P414_MAGIC &&
	    old.version == A52_P414_VERSION &&
	    old.phase == 414U &&
	    old.boot_id && old.boot_id != ~0ULL)
		a52_p414_boot_id = old.boot_id + 1ULL;
	else
		a52_p414_boot_id = 1ULL;

	a52_p414_seq = 0;
	a52_p414_disk_count = 0;
	a52_p414_ram_count = 0;
	a52_p414_dropped = 0;
	a52_p414_state = A52_P414_STATE_DISK;
	a52_p414_flushed_full_pages = 0;
	a52_p416_disk_armed = 0;
	a52_p416_worker_runs = 0;
	a52_p416_open_rc = -EAGAIN;
	a52_p416_submit_rc = -EAGAIN;
	a52_p416_flush_rc = -EAGAIN;
	a52_p416_last_page = 0;

	memset(a52_p414_disk_stage, 0, A52_P414_DISK_BYTES);
	memset_io(a52_p414_ram, 0, A52_P414_HEADER_BYTES);

	a52_p414_fill_header(&h);
	memcpy(a52_p414_disk_stage, &h, sizeof(h));
	memcpy_toio(a52_p414_ram, &h, sizeof(h));
	a52_p406_persist(a52_p414_ram, sizeof(h));
	a52_p416_sync_status_locked();

	atomic_set(&a52_p414_ready, 1);
	/* Phase416: stage now, arm raw block I/O only from late_initcall. */

	/* This same record is admitted into R48, tying both channels to boot_id. */
	a52_ackfr_record("P414 BOOT id=%llu disk=%u ram=%u",
		(unsigned long long)a52_p414_boot_id,
		A52_P414_DISK_CAPACITY, A52_P414_RAM_CAPACITY);
	a52_ackfr_record("P418 CTRL boot ret=%u",
		a52_ackfr_phase418_retained());
	a52_ackfr_record("P276 394T boot=1 ret=%u",
		a52_ackfr_phase418_retained());
	return 0;
}
core_initcall_sync(a52_p414_init);


static int __init a52_p416_disk_late_arm(void)
{
	unsigned long flags;

	if (!atomic_read(&a52_p414_ready))
		return 0;

	spin_lock_irqsave(&a52_p414_lock, flags);
	WRITE_ONCE(a52_p416_disk_armed, 1U);
	a52_p416_sync_status_locked();
	spin_unlock_irqrestore(&a52_p414_lock, flags);

	/* Flush everything staged since core_init, even if no later event arrives. */
	a52_p414_queue_disk();
	a52_ackfr_record("P416 DISKARM gen=%u count=%u",
		(unsigned int)atomic_read(&a52_p414_disk_gen),
		a52_p414_disk_count);
	return 0;
}
late_initcall_sync(a52_p416_disk_late_arm);


void a52_ackfr_record(const char *fmt, ...)
{
	struct a52_r179_event event;
	unsigned long irq_flags;
	unsigned int written;
	bool buffered;
	bool critical;
	va_list args;
	u64 seq;

	/* A52_PHASE234_RSCC_FOCUSED_RECORDER_V1
	 * A52_PHASE235_RSCC_MASTER_RECORDER_V1
	 * A52_PHASE236_DISPLAY_INIT_RECORDER_V1
	 * A52_PHASE237_OFPOP_PLATFORM_PROBE_RECORDER_V1
	 * A52_PHASE238_BROAD_GPU_SUPPLIER_RECORDER_V1
	 * A52_PHASE239_GPU_CX_VDD_PARENT_IDENTITY_V1
	 * A52_PHASE240_CX_FROZEN_LATCH_V1
	 * A52_PHASE240_CX_SUPPLIER_GATE_LATCH_IDENTITY_V1
	 * A52_PHASE241_CX_BROAD_CORRIDOR_LATCH_V1
	 * A52_PHASE241_CX_BROAD_CORRIDOR_LATCH_IDENTITY_V1
	 * A52_PHASE242_CX_STICKY_STATE_V1
	 * A52_PHASE242_CX_STICKY_STATE_IDENTITY_V1
	 * A52_PHASE243_CXGX_LIVE_SUPPLIER_V1
	 * A52_PHASE243_CXGX_LIVE_SUPPLIER_IDENTITY_V1
	 * A52_PHASE246_SUBSYS_INITCALL_CORRIDOR_V1
	 * A52_PHASE243_PHASE242_RUNTIME_DISABLED_V1
	 * BOOT rs=ready phase=234 focus=rscc compatibility marker only.
	 * Phase 235 preserves the Phase 210 R48/RS48/CRC32C wire format.
	 * Persist only RSCC plus the inherited bounded DRM component-master
	 * records so the useful retention window is not consumed by unrelated
	 * boot/GPU/driver-core traffic.
	 */
	if (!fmt)
		return;
	/* A52_PHASE331_POST_RETENTION_FRONTIER_RECORDER_V1
	 * Phase280 still freezes the recorder globally.  Only Phase331's tightly
	 * scoped post-timeout breadcrumbs are admitted after that freeze.
	 */
	/* A52_PHASE332_PERSISTENT_GDM_TIMEOUT_FRONTIER_V1
	 * Phase331 admitted only P276 331* after the Phase280 freeze. Phase332
	 * additionally admits P276 332* so the persistent exact-F0 GDM latch can
	 * carry timeout-frontier breadcrumbs across workqueue/task boundaries.
	 */
	/* A52_PHASE339_WARM_REBOOT_PRESERVATION_PROBE_V1
	 * Preserve the Phase280 freeze. Admit only P276 339* in addition to the
	 * existing 331/332 timeout breadcrumbs so late preservation checkpoints can
	 * survive if exact-F0 freezes ordinary traffic before the warm reboot.
	 */
	/* A52_PHASE340_POST12_RUNTIME_DISCRIMINATOR_V1
	 * Phase340 additionally admits only P276 340*. The 340 stream comes from a
	 * dedicated kthread/PM notifier and is independent of system delayed_work.
	 */
	if (unlikely(atomic_read(&a52_r280_retained)) &&
	    strncmp(fmt, "U446", 4) &&
	    strncmp(fmt, "P440", 4) &&
	    strncmp(fmt, "P439", 4) &&
	    strncmp(fmt, "P434", 4) &&
    strncmp(fmt, "P433", 4) &&
    strncmp(fmt, "P432", 4) &&
	    strncmp(fmt, "P431", 4) &&
	    strncmp(fmt, "P430", 4) &&
	    strncmp(fmt, "P429", 4) &&
	    strncmp(fmt, "P427", 4) &&
	    strncmp(fmt, "P426", 4) &&
	    strncmp(fmt, "P425", 4) &&
	    strncmp(fmt, "P424", 4) &&
	    strncmp(fmt, "P423", 4) &&
	    strncmp(fmt, "P420", 4) &&
	    strncmp(fmt, "P419", 4) &&
	    strncmp(fmt, "P418", 4) &&
	    !a52_r382_diag_format(fmt) &&
	    !(fmt[0] == 'P' && fmt[1] == '2' && fmt[2] == '7' &&
	      fmt[3] == '6' && fmt[4] == ' ' &&
	      ((fmt[5] == '3' && fmt[6] == '3' &&
	        (fmt[7] == '1' || fmt[7] == '2' || fmt[7] == '9')) ||
	       (fmt[5] == '3' && fmt[6] == '4' && fmt[7] == '0')))) {
		if (!strncmp(fmt, "P276 387", 9) ||
		    !strncmp(fmt, "P276 394", 9))
			a52_p418_retention_drop_note(fmt);
		return;
	}
	/* A52_PHASE248_KGSL_GMU_IOMMU_CORRIDOR_V1 */
/* A52_PHASE249_GPU_SMMU_ENODEV_ROOT_V1 */
/* A52_PHASE250_GPU_SMMU_POWER_CONTRACT_V1 */
/* A52_PHASE251_GMU_POST_MMIO_TAIL_DIAG_V1 */
/* A52_PHASE255_POSTBOOT_VISIBILITY_V1
 * Visibility only. Restore previously compiled post-boot recorder prefixes
 * that Phase234's focused admission gate suppressed. TRIPOST is the sparse
 * cumulative checkpoint; BOOTPOST/USRPOST/ODSPOST/GFXPOST feed its state.
 */
/* A52_PHASE256_KGSL_DEVNODE_FRAMEWORK_V1
 * Retain the narrow kgsl-devnode/framework milestone stream after capacity.
 */
/* A52_PHASE257_KGSL_PUBLICATION_PIPELINE_V1
 * Retain the Phase257 KGSL publication/coldboot/node-creation stream.
 */
/* A52_PHASE259_RECORDER_V1
 * Retain the Phase259 target-only KGSL node lifetime stream.
 */
/* A52_PHASE260_RECORDER_V1
 * Retain the Phase260 VFS-to-KGSL handoff bridge.
 */
/* A52_PHASE261_RECORDER_V1 */
/* A52_PHASE267_PREDRM_DIRECT_BOUNDARY_V3: direct boundary diagnostics. */
/* A52_PHASE267S_DISPLAY_LIFECYCLE_ADMISSION_V1: reuse existing low-volume scopes. */
if (strncmp(fmt, "U446", 4) &&
    strncmp(fmt, "P440", 4) &&
	    strncmp(fmt, "P439", 4) &&
	    strncmp(fmt, "P434", 4) &&
    strncmp(fmt, "P433", 4) &&
    strncmp(fmt, "P432", 4) &&
	    strncmp(fmt, "P431", 4) &&
	    strncmp(fmt, "P430", 4) &&
    strncmp(fmt, "P429", 4) &&
    strncmp(fmt, "P427", 4) &&
    strncmp(fmt, "P426", 4) &&
    strncmp(fmt, "P425", 4) &&
    strncmp(fmt, "P424", 4) &&
    strncmp(fmt, "P423", 4) &&
    strncmp(fmt, "P420", 4) &&
    strncmp(fmt, "P419", 4) &&
    strncmp(fmt, "P418", 4) &&
    strncmp(fmt, "P417", 4) &&
    strncmp(fmt, "P416", 4) &&
    strncmp(fmt, "P415", 4) &&
    strncmp(fmt, "P414", 4) &&
    strncmp(fmt, "DISP PROBE", 10) &&
    strncmp(fmt, "DISP DEV", 8) &&
    strncmp(fmt, "DISP BIND", 9) &&
    strncmp(fmt, "DISP CTRL", 9) &&
    strncmp(fmt, "DISP PREP", 9) &&
    strncmp(fmt, "DISP PANEL_", 11) &&
    strncmp(fmt, "DISP ENABLE", 11) &&
    strncmp(fmt, "DISP POST", 9) &&
    strncmp(fmt, "DISP MODE", 9) &&
    strncmp(fmt, "DISP PIN", 8) &&
    strncmp(fmt, "DISP POWER", 10) &&
    strncmp(fmt, "DISP RESET", 10) &&
    strncmp(fmt, "DISP SS_", 8) &&
    strncmp(fmt, "DISP CMDSET", 11) &&
    strncmp(fmt, "P276", 4) &&
    strncmp(fmt, "P275", 4) &&
    strncmp(fmt, "P274", 4) &&
    strncmp(fmt, "P273", 4) &&
    strncmp(fmt, "P271", 4) &&
    strncmp(fmt, "P270", 4) &&
    strncmp(fmt, "P268", 4) &&
    strncmp(fmt, "DRMPOST 211", 11) &&
    strncmp(fmt, "DRMPOST 212", 11) &&
    strncmp(fmt, "P267", 4) &&
    strcmp(fmt, "%s enter fn=%s") &&
    strcmp(fmt, "%s exit fn=%s us=%llu") &&
    strncmp(fmt, "KMSPOST", 7) &&
    strncmp(fmt, "F261", 4) &&
    strncmp(fmt, "F260", 4) &&
    strncmp(fmt, "F259", 4) &&
    strncmp(fmt, "F257", 4) &&
    strncmp(fmt, "F256", 4) &&
    strncmp(fmt, "K255VIS", 7) &&
    strncmp(fmt, "TRIPOST", 7) &&
    strncmp(fmt, "BOOTPOST", 8) &&
    strncmp(fmt, "USRPOST", 7) &&
    strncmp(fmt, "ODSPOST", 7) &&
    strncmp(fmt, "GFXPOST", 7) &&
    strncmp(fmt, "K254", 4) &&
    strncmp(fmt, "K253", 4) &&
    strncmp(fmt, "K251", 4) &&
    strncmp(fmt, "K250", 4) &&
    strncmp(fmt, "K249", 4) &&
    strncmp(fmt, "K248", 4) &&
    strncmp(fmt, "CXF246", 6) &&
	    strncmp(fmt, "CXF243", 6) &&
	    strncmp(fmt, "CXF242", 6) &&
	    strncmp(fmt, "CXF241", 6) &&
	    strncmp(fmt, "CXF240", 6) &&
	    strncmp(fmt, "A52GDSC", 7) &&
	    strncmp(fmt, "G238", 4) &&
	    strncmp(fmt, "KGPPOST", 7) &&
	    strncmp(fmt, "OFPOP", 5) &&
	    strncmp(fmt, "P3P", 3) &&
	    strncmp(fmt, "DISPINIT", 8) &&
	    strncmp(fmt, "RSCC", 4) &&
	    strncmp(fmt, "DRMCOMP", 7) &&
	    strncmp(fmt, "COMP ", 5) &&
	    strncmp(fmt, "BOOT ctl=", 9) &&
	    strncmp(fmt, "BOOT rs=ready", 13) &&
	    strncmp(fmt, "BOOT phase=", 11) &&
	    strncmp(fmt, "U380", 4) &&
	    strncmp(fmt, "B380", 4) &&
	    strncmp(fmt, "L380", 4) &&
	    strncmp(fmt, "USB380", 6) &&
	    strncmp(fmt, "V380", 4) &&
	    strncmp(fmt, "V381", 4) &&
	    strncmp(fmt, "V382", 4) &&
	    strncmp(fmt, "P383", 4) &&
	    strncmp(fmt, "L383", 4) &&
	    strncmp(fmt, "U384", 4) &&
	    strncmp(fmt, "B384", 4) &&
	    strncmp(fmt, "L384", 4) &&
	    strncmp(fmt, "P385", 4) &&
	    strncmp(fmt, "B385", 4) &&
	    strncmp(fmt, "V385", 4) &&
	    strncmp(fmt, "P393", 4) &&
	    strncmp(fmt, "R393", 4) &&
	    strncmp(fmt, "M393", 4) &&
	    strncmp(fmt, "D393", 4) &&
	    strncmp(fmt, "I393", 4))
		return;

	seq = (u64)atomic64_inc_return(&a52_r179_sequence);
	buffered = seq <= A52_R179_CAPACITY;
	if (!buffered)
		atomic64_inc(&a52_r179_dropped);

	memset(&event, 0, sizeof(event));
	event.seq = seq;
	event.monotonic_ns = ktime_get_ns();
	event.pid = current->pid;
	event.tgid = current->tgid;
	event.cpu = task_cpu(current);
	/* A52_FENCE1D_ACKFR_IRQ_COMM_SNAPSHOT_V1: no task_lock in IRQ-reentrant recorder. */
	memcpy(event.comm, current->comm, TASK_COMM_LEN);
	event.comm[TASK_COMM_LEN - 1] = '\0';
	/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: suppress unrelated forensic traffic. */
	if (!fmt || (
	    strncmp(fmt, "P440", 4) &&
	    strncmp(fmt, "P439", 4) &&
	    strncmp(fmt, "P434", 4) &&
    strncmp(fmt, "P433", 4) &&
    strncmp(fmt, "P432", 4) &&
	    strncmp(fmt, "P431", 4) &&
	    strncmp(fmt, "P430", 4) &&
	    strncmp(fmt, "P429", 4) &&
	    strncmp(fmt, "P427", 4) &&
	    strncmp(fmt, "P426", 4) &&
	    strncmp(fmt, "P425", 4) &&
	    strncmp(fmt, "P424", 4) &&
	    strncmp(fmt, "P423", 4) &&
	    strncmp(fmt, "P420", 4) &&
	    strncmp(fmt, "P419", 4) &&
	    strncmp(fmt, "P418", 4) &&
	    strncmp(fmt, "P417", 4) &&
	    strncmp(fmt, "P416", 4) &&
	    strncmp(fmt, "P415", 4) &&
	    strncmp(fmt, "P414", 4) &&
	    strncmp(fmt, "P273", 4) &&
	    strncmp(fmt, "P268", 4) &&
	    strncmp(fmt, "P267", 4) &&
	    strncmp(fmt, "DRMPOST 212", 11) &&
	    strncmp(fmt, "KMSPOST", 7) &&
	    strncmp(fmt, "DISP PROBE", 10) &&
	    strncmp(fmt, "DISP DEV", 8) &&
	    strncmp(fmt, "DISP BIND", 9) &&
	    strncmp(fmt, "DISP CTRL", 9) &&
	    strncmp(fmt, "DISP PREP", 9) &&
	    strncmp(fmt, "DISP PANEL_", 11) &&
	    strncmp(fmt, "DISP ENABLE", 11) &&
	    strncmp(fmt, "DISP POST", 9) &&
	    strncmp(fmt, "DISP MODE", 9) &&
	    strncmp(fmt, "DISP PIN", 8) &&
	    strncmp(fmt, "DISP POWER", 10) &&
	    strncmp(fmt, "DISP RESET", 10) &&
	    strncmp(fmt, "DISP SS_", 8) &&
	    strncmp(fmt, "DISP CMDSET", 11) &&
	    strcmp(fmt, "%s enter fn=%s") &&
	    strcmp(fmt, "%s exit fn=%s us=%llu") &&
	    strncmp(fmt, "P402 ", 5) &&
	    strncmp(fmt, "P276 280", 9) &&
	    strncmp(fmt, "P276 303", 9) &&
	    strncmp(fmt, "P276 307", 9) &&
	    strncmp(fmt, "P276 312", 9) &&
	    strncmp(fmt, "P276 314", 9) &&
	    strncmp(fmt, "P276 316", 9) &&
	    strncmp(fmt, "P276 319", 9) &&
	    strncmp(fmt, "P276 323", 9) &&
	    strncmp(fmt, "P276 328", 9) &&
	    strncmp(fmt, "P276 329", 9) &&
	    strncmp(fmt, "P276 330", 9) &&
	    strncmp(fmt, "P276 345", 9) &&
	    strncmp(fmt, "P276 346", 9) &&
	    strncmp(fmt, "P276 347", 9) &&
	    strncmp(fmt, "P276 348", 9) &&
	    strncmp(fmt, "P276 349", 9) &&
	    strncmp(fmt, "P276 387", 9) &&
	    strncmp(fmt, "P276 394", 9)))
		return;
	va_start(args, fmt);
	vscnprintf(event.message, sizeof(event.message), fmt, args);
	/* A52_PHASE392_PERSISTENT_GAP_MIRROR_CALL_V1 */
	a52_p392_gap_write(event.message);
	a52_p414_append_text(event.message); /* A52_PHASE414_SEQUENTIAL_3M_RECORDER_V1 */
	/* Phase421 owns the Phase405 fixed slots; legacy mirror retired. */
	va_end(args);
	/* A52_PHASE243_PHASE242_RUNTIME_DISABLED_V1: no sticky parsing in Phase 243 */
	a52_r241_corridor_latch(event.message);
	a52_r240_cxf_latch(event.message);
	a52_r230_journal_message(event.message);
	a52_r228_track_message(event.message);
	a52_r273_track_message(event.message);
	critical = a52_r179_is_critical_message(event.message);
	if (!buffered && !critical)
		return;

	if (buffered) {
		spin_lock_irqsave(&a52_r179_lock, irq_flags);
		a52_r179_events[seq - 1] = event;
		spin_unlock_irqrestore(&a52_r179_lock, irq_flags);
	}

	written = a52_r179_persist_event(&event, A52_R179_BANK_ALL);
	if (written && buffered) {
		spin_lock_irqsave(&a52_r179_lock, irq_flags);
		a52_r179_events[seq - 1].persisted_mask |= written;
		spin_unlock_irqrestore(&a52_r179_lock, irq_flags);
	}
	if (written && atomic_cmpxchg(&a52_r179_backend_seen, 0, 1) == 0) {
		a52_r179_write_control("BOOT_BEGIN");
		a52_r179_flush_missing(seq);
	}
}
EXPORT_SYMBOL_GPL(a52_ackfr_record);

void a52_ackfr_retain_timeout_snapshot(void)
{
	atomic_set(&a52_r280_retained, 1);
}
EXPORT_SYMBOL_GPL(a52_ackfr_retain_timeout_snapshot);

struct a52_ackfr_scope a52_ackfr_scope_begin(const char *domain,
					      const char *name)
{
	struct a52_ackfr_scope scope = {
		.domain = domain,
		.name = name,
		.start_ns = ktime_get_ns(),
	};

	a52_ackfr_record("%s enter fn=%s", domain, name);
	return scope;
}
EXPORT_SYMBOL_GPL(a52_ackfr_scope_begin);

void a52_ackfr_scope_cleanup(struct a52_ackfr_scope *scope)
{
	u64 duration_us;

	if (!scope || !scope->start_ns)
		return;
	duration_us = div_u64(ktime_get_ns() - scope->start_ns, 1000U);
	a52_ackfr_record("%s exit fn=%s us=%llu", scope->domain, scope->name,
			  (unsigned long long)duration_us);
}
EXPORT_SYMBOL_GPL(a52_ackfr_scope_cleanup);

static int __init __maybe_unused a52_r179_rs_init(void)
{
	a52_r179_rs = init_rs(8, 0x11d, 0, 1, A52_R179_RS_ROOTS);
	if (!a52_r179_rs) {
		pr_err("failed to initialize Reed-Solomon codec\n");
		return -ENOMEM;
	}
	smp_wmb();
	a52_ackfr_record("BOOT rs=ready phase=243 focus=cx-gdsc-own-suppliers roots=%u copies=3 crc=crc32c",
			 A52_R179_RS_ROOTS);
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired postcore_initcall(a52_r179_rs_init); */

#define A52_R179_HEARTBEAT_INTERVAL_MS 1000U
#define A52_R179_HEARTBEAT_LIMIT 180U

/* A52_PHASE226_ODSIGN_GATE_TRACE */
static bool a52_r226_snapshot_tick(unsigned int tick)
{
	return tick == 20 || tick == 30 || tick == 45 || tick == 60 ||
	       tick == 90 || tick == 120 || tick == 150 || tick == 180;
}

static bool a52_r226_od_task(const char *comm)
{
	return comm && (!strcmp(comm, "odsign") ||
			!strcmp(comm, "odrefresh"));
}

static void a52_r226_task_snapshot(unsigned int tick)
{
	struct task_struct *group;
	struct task_struct *task;
	unsigned int found = 0;

	if (!a52_r226_snapshot_tick(tick))
		return;
	rcu_read_lock();
	for_each_process_thread(group, task) {
		if (!a52_r226_od_task(task->comm))
			continue;
		found++;
		a52_ackfr_record("ODSPOST 226 ts t=%u p=%d c=%.10s s=%lx x=%x r=%d o=%d i=%d",
			tick, task_pid_nr(task), task->comm,
			READ_ONCE(task->state), READ_ONCE(task->exit_state),
			READ_ONCE(task->on_rq), READ_ONCE(task->on_cpu),
			task->in_iowait);
	}
	rcu_read_unlock();
	if (!found)
		a52_ackfr_record("ODSPOST 226 ts t=%u none", tick);
}


enum a52_r273_task_id {
	A52_R273_INIT = 0,
	A52_R273_SM,
	A52_R273_HSM,
	A52_R273_VSM,
	A52_R273_VOLD,
	A52_R273_KEYSTORE,
	A52_R273_SF,
	A52_R273_Z64,
	A52_R273_Z32,
	A52_R273_SYSTEM_SERVER,
	A52_R273_BOOTANIM,
	A52_R273_SYSTEMUI,
	A52_R273_NETD,
	A52_R273_AUDIO,
};

struct a52_r273_task_sample {
	pid_t pid;
	pid_t tgid;
	pid_t ppid;
	char comm[TASK_COMM_LEN];
};

static const char * const a52_r273_task_key[A52_R273_TASK_COUNT] = {
	"I", "SM", "HSM", "VSM", "V", "K", "SF",
	"Z64", "Z32", "SS", "BA", "UI", "N", "A",
};
static pid_t a52_r273_last_pid[A52_R273_TASK_COUNT];
static u32 a52_r273_ever_mask;
static u32 a52_r273_gone_mask;
static unsigned int a52_r273_last_summary_bucket;

static int a52_r273_task_id(const char *comm)
{
	if (!comm)
		return -1;
	if (!strcmp(comm, "init"))
		return A52_R273_INIT;
	if (!strcmp(comm, "servicemanager"))
		return A52_R273_SM;
	if (!strncmp(comm, "hwservicemanage", 15))
		return A52_R273_HSM;
	if (!strncmp(comm, "vndservicemanag", 15))
		return A52_R273_VSM;
	if (!strcmp(comm, "vold"))
		return A52_R273_VOLD;
	if (!strcmp(comm, "keystore2"))
		return A52_R273_KEYSTORE;
	if (!strcmp(comm, "surfaceflinger"))
		return A52_R273_SF;
	if (!strcmp(comm, "zygote64"))
		return A52_R273_Z64;
	if (!strcmp(comm, "zygote"))
		return A52_R273_Z32;
	if (!strcmp(comm, "system_server"))
		return A52_R273_SYSTEM_SERVER;
	if (!strcmp(comm, "bootanimation"))
		return A52_R273_BOOTANIM;
	if (!strncmp(comm, "com.android.sys", 15))
		return A52_R273_SYSTEMUI;
	if (!strcmp(comm, "netd"))
		return A52_R273_NETD;
	if (!strcmp(comm, "audioserver"))
		return A52_R273_AUDIO;
	return -1;
}

static void a52_r273_scan_tasks(unsigned long boot_s)
{
	struct a52_r273_task_sample now[A52_R273_TASK_COUNT];
	struct task_struct *task;
	unsigned int now_mask = 0;
	unsigned int bucket;
	unsigned long flags;
	char comm[TASK_COMM_LEN];
	char disp_fn[40];
	char disp_kind;
	pid_t zpid;
	unsigned int i;
	int id;

	memset(now, 0, sizeof(now));
	rcu_read_lock();
	for_each_process(task) {
		get_task_comm(comm, task);
		id = a52_r273_task_id(comm);
		if (id < 0 || now[id].pid)
			continue;
		now[id].pid = task_pid_nr(task);
		now[id].tgid = task_tgid_nr(task);
		now[id].ppid = task_ppid_nr(task);
		strscpy(now[id].comm, comm, sizeof(now[id].comm));
		now_mask |= BIT(id);
	}
	rcu_read_unlock();

	for (i = 0; i < A52_R273_TASK_COUNT; i++) {
		if (now[i].pid && !a52_r273_last_pid[i]) {
			a52_ackfr_record("P273 U + k=%s p=%d t=%d pp=%d c=%.15s",
				a52_r273_task_key[i], now[i].pid, now[i].tgid,
				now[i].ppid, now[i].comm);
			a52_r273_ever_mask |= BIT(i);
		} else if (!now[i].pid && a52_r273_last_pid[i]) {
			a52_ackfr_record("P273 U - k=%s p=%d",
				a52_r273_task_key[i], a52_r273_last_pid[i]);
			a52_r273_gone_mask |= BIT(i);
		} else if (now[i].pid && a52_r273_last_pid[i] != now[i].pid) {
			a52_ackfr_record("P273 U R k=%s o=%d n=%d pp=%d",
				a52_r273_task_key[i], a52_r273_last_pid[i],
				now[i].pid, now[i].ppid);
			a52_r273_ever_mask |= BIT(i);
			a52_r273_gone_mask |= BIT(i);
		}
		a52_r273_last_pid[i] = now[i].pid;
	}

	bucket = (unsigned int)(boot_s / A52_R273_SUMMARY_S);
	if (!bucket || bucket == a52_r273_last_summary_bucket)
		return;
	a52_r273_last_summary_bucket = bucket;
	zpid = now[A52_R273_Z64].pid ? now[A52_R273_Z64].pid :
		now[A52_R273_Z32].pid;

	a52_ackfr_record("P273 F t=%lu n=%x e=%x g=%x f=%d z=%d s=%d u=%d b=%d",
		boot_s, now_mask, a52_r273_ever_mask, a52_r273_gone_mask,
		now[A52_R273_SF].pid, zpid, now[A52_R273_SYSTEM_SERVER].pid,
		now[A52_R273_SYSTEMUI].pid, now[A52_R273_BOOTANIM].pid);

	spin_lock_irqsave(&a52_r273_disp_lock, flags);
	strscpy(disp_fn, a52_r273_last_disp_fn, sizeof(disp_fn));
	disp_kind = a52_r273_last_disp_kind;
	spin_unlock_irqrestore(&a52_r273_disp_lock, flags);
	a52_ackfr_record("P273 D t=%lu n=%d k=%c fn=%.36s", boot_s,
		atomic_read(&a52_r273_disp_count), disp_kind,
		disp_fn[0] ? disp_fn : "none");
	a52_ackfr_record("P273 R t=%lu kd=%d nm=%d e=%d s=%d", boot_s,
		atomic_read(&a52_r273_kd_status), atomic_read(&a52_r273_mode_count),
		atomic_read(&a52_r273_encoder_id), atomic_read(&a52_r273_selected_id));
}

/* A52_PHASE274_FRONTIER_TIMEBASE_FIX_V1
 * Phase273 used absolute unsigned jiffies as seconds-since-boot. Linux
 * intentionally biases initial_jiffies near wraparound, so the first
 * frontier pass looked older than the 900-second horizon and stopped.
 * Use wrap-safe elapsed jiffies from the moment the worker is armed.
 * Diagnostic timing only; no device or userspace behavior is changed.
 */
/* A52_PHASE275_LEVEL1_KEY_SEND_FRONTIER_V1
 * Phase274 hardware isolated the first non-returning panel operation to
 * ss_send_cmd(TX_LEVEL1_KEY_ENABLE). P275 admits sparse target-only
 * checkpoints while preserving the R48/RS48 wire format.
 */
/* A52_PHASE276_DSI_PANEL_TRANSFER_FRONTIER_V1
 * Phase275 hardware reached dsi_panel_tx_cmd_set(TX_LEVEL1_KEY_ENABLE)
 * after successful PM and DSI clock-on. P276 admits target-only internal
 * command-lock and host-transfer checkpoints. R48/RS48 stays unchanged.
 */
static unsigned long a52_r274_frontier_start_jiffies;


/* A52_PHASE418_ATOMIC_PRESENT_DISCRIMINATOR_V1
 * Low-rate process frontier for the long pre-zygote gap. One sample/10 s.
 */
static unsigned int a52_p418_apex_bucket;

static void __maybe_unused a52_p418_apex_sample(unsigned long boot_s)
{
	struct task_struct *task;
	unsigned int bucket = (unsigned int)(boot_s / 10UL);
	unsigned int n = 0;
	pid_t pid = 0;
	long state = 0;
	int cpu = -1;
	char comm[TASK_COMM_LEN] = "-";
	char tmp[TASK_COMM_LEN];

	if (!bucket || bucket == a52_p418_apex_bucket || boot_s > 180UL)
		return;
	a52_p418_apex_bucket = bucket;

	rcu_read_lock();
	for_each_process(task) {
		get_task_comm(tmp, task);
		if (strncmp(tmp, "apexd", 5))
			continue;
		n++;
		if (!pid) {
			pid = task_pid_nr(task);
			state = READ_ONCE(task->state);
			cpu = task_cpu(task);
			strscpy(comm, tmp, sizeof(comm));
		}
	}
	rcu_read_unlock();

	a52_ackfr_record("P418 APX t=%lu n=%u p=%d st=%lx cpu=%d c=%.15s",
		boot_s, n, pid, state, cpu, comm);
}

static void a52_r273_frontier_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(a52_r273_frontier_work, a52_r273_frontier_fn);

static void a52_r273_frontier_fn(struct work_struct *work)
{
	unsigned long elapsed_j = jiffies - a52_r274_frontier_start_jiffies;
	unsigned long boot_s = jiffies_to_msecs(elapsed_j) / 1000U;
	unsigned int delay_ms;

	a52_r273_scan_tasks(boot_s);
	/* Phase429: periodic P418 APX text sampler retired; binary census owns this. */
	if (boot_s >= A52_R273_FRONTIER_END_S)
		return;
	delay_ms = boot_s < 300U ? A52_R273_SCAN_FAST_MS : A52_R273_SCAN_SLOW_MS;
	schedule_delayed_work(&a52_r273_frontier_work,
		msecs_to_jiffies(delay_ms));
}


/* A52_PHASE417_POSTBIND_FRONTIER_V1
 * Diagnostic-only: re-arm the existing low-rate task frontier after late init.
 * It tracks init/servicemanager/surfaceflinger/zygote/system_server/bootanimation
 * without restoring any Phase380 UFS sampler or Phase395/396 NoC observer.
 */
static int __init a52_p417_frontier_init(void)
{
	memset(a52_r273_last_pid, 0, sizeof(a52_r273_last_pid));
	a52_r273_ever_mask = 0U;
	a52_r273_gone_mask = 0U;
	a52_r273_last_summary_bucket = 0U;
	a52_r274_frontier_start_jiffies = jiffies;
	schedule_delayed_work(&a52_r273_frontier_work, msecs_to_jiffies(250));
	a52_ackfr_record("P417 FRONTIER arm delay=250");
	return 0;
}
late_initcall_sync(a52_p417_frontier_init);

/* A52_PHASE339_WARM_REBOOT_PRESERVATION_PROBE_V1
 * This is a preservation-only experiment.  It does not touch DRM, DSI, clocks,
 * MMIO, regulators, resets or atomic return values.  Checkpoints straddle the
 * observed ~11.932 s recovery frontier and continue to five minutes.  The final
 * worker forces a warm reboot directly into recovery so no manual hard-reset
 * path participates in the handoff.
 */
static const unsigned int a52_r339_checkpoints_s[] = {
	10U, 12U, 15U, 30U, 60U, 120U, 180U, 240U, 300U, 330U,
};

/* A52_PHASE340_POST12_RUNTIME_DISCRIMINATOR_V1
 *
 * Independent sideband + dedicated-kthread discriminator for the repeatable
 * ~11.932 s frontier. The sideband occupies the final 2 KiB of the pmsg zone;
 * current recovery captures show the pmsg zone using only ~1 KiB, while R48
 * itself never targets pmsg. Each 64-byte slot is cache-cleaned to PoC after
 * commit so recovery can inspect this path independently of the R48 backend.
 */
#define A52_R340_SIDEBAND_PHYS 0xB1BFF800ULL
#define A52_R340_SIDEBAND_BYTES 0x800U
#define A52_R340_SLOT_BYTES 64U
#define A52_R340_SLOT_COUNT (A52_R340_SIDEBAND_BYTES / A52_R340_SLOT_BYTES)
#define A52_R340_MAGIC 0x3034335244353241ULL
#define A52_R340_COMMIT 0x340c0de5U

enum a52_r340_event {
	A52_R340_EVT_ARM = 1,
	A52_R340_EVT_KTHREAD = 2,
	A52_R340_EVT_PM = 3,
	A52_R340_EVT_REBOOT = 4,
};

struct a52_r340_slot {
	u64 magic;
	u64 monotonic_ns;
	u64 sequence;
	u64 jiffies64;
	u32 version;
	u32 index;
	u32 event;
	u32 arg;
	u32 retained;
	u32 cpu;
	u32 commit;
	u32 reserved;
};

static void *a52_r340_sideband;
static atomic_t a52_r340_sideband_index = ATOMIC_INIT(0);
static struct task_struct *a52_r340_task;

static void a52_r340_sideband_write(u32 event, u32 arg)
{
	struct a52_r340_slot slot;
	void *dst;
	unsigned int index;

	if (!READ_ONCE(a52_r340_sideband))
		return;

	memset(&slot, 0, sizeof(slot));
	index = (unsigned int)atomic_inc_return(&a52_r340_sideband_index) - 1U;
	slot.magic = A52_R340_MAGIC;
	slot.monotonic_ns = ktime_get_ns();
	slot.sequence = (u64)atomic64_read(&a52_r179_sequence);
	slot.jiffies64 = get_jiffies_64();
	slot.version = 1U;
	slot.index = index;
	slot.event = event;
	slot.arg = arg;
	slot.retained = (u32)atomic_read(&a52_r280_retained);
	slot.cpu = (u32)task_cpu(current);
	slot.commit = A52_R340_COMMIT;

	dst = (u8 *)a52_r340_sideband +
		(index % A52_R340_SLOT_COUNT) * A52_R340_SLOT_BYTES;
	memcpy(dst, &slot, sizeof(slot));
	wmb();
	__flush_dcache_area(dst, sizeof(slot));
}

static int a52_r340_pm_notify(struct notifier_block *nb,
			      unsigned long action, void *unused)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		a52_r340_sideband_write(A52_R340_EVT_PM, (u32)action);
		a52_ackfr_record("P276 340P e=%lu q=%llu r=%u",
			action,
			(unsigned long long)atomic64_read(&a52_r179_sequence),
			(unsigned int)atomic_read(&a52_r280_retained));
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block a52_r340_pm_nb = {
	.notifier_call = a52_r340_pm_notify,
};

static int a52_r340_thread_fn(void *unused)
{
	static const unsigned int checkpoints_s[] = {
		10U, 12U, 15U, 30U, 60U,
	};
	unsigned int i;
	unsigned int previous = 0;

	for (i = 0; i < ARRAY_SIZE(checkpoints_s); i++) {
		unsigned int now = checkpoints_s[i];

		if (kthread_should_stop())
			return 0;
		msleep((now - previous) * 1000U);
		a52_r340_sideband_write(A52_R340_EVT_KTHREAD, now);
		a52_ackfr_record("P276 340K t=%u q=%llu r=%u",
			now,
			(unsigned long long)atomic64_read(&a52_r179_sequence),
			(unsigned int)atomic_read(&a52_r280_retained));
		previous = now;
	}

	/* A52_PHASE363_REMOVE_60S_REBOOT_V1
	 * Phase362 proved UFS is no longer the boot wall. Do not terminate
	 * the experiment at 60 s; leave the inherited Phase339 330 s safety
	 * reboot intact so late Android/display progress can be observed.
	 */
	a52_ackfr_record("P276 340C t=60 continue=1");
	return 0;
}

static void __used a52_r340_start(void)
{
	long kt_rc = 0;
	int pm_rc;

	BUILD_BUG_ON(sizeof(struct a52_r340_slot) != A52_R340_SLOT_BYTES);
	a52_r340_sideband = memremap(A52_R340_SIDEBAND_PHYS,
		A52_R340_SIDEBAND_BYTES, MEMREMAP_WB);
	pm_rc = register_pm_notifier(&a52_r340_pm_nb);
	a52_r340_task = kthread_run(a52_r340_thread_fn, NULL,
				    "a52-p340");
	if (IS_ERR(a52_r340_task)) {
		kt_rc = PTR_ERR(a52_r340_task);
		a52_r340_task = NULL;
	}

	a52_r340_sideband_write(A52_R340_EVT_ARM, 0U);
	a52_ackfr_record("P276 340A map=%u pm=%d kt=%ld",
		a52_r340_sideband ? 1U : 0U, pm_rc, kt_rc);
}

/* A52_PHASE341_HARDIRQ_TIMER_FRONTIER_V1
 *
 * Hard-IRQ timer discriminator for the repeatable ~11.9 s runtime frontier.
 * Three pinned hrtimers (CPU0, CPU5, CPU7) write only to an independent raw
 * sideband every 500 ms.  This path does not depend on system_wq, kthreads or
 * the R48 recorder.  Each event is mirrored into two halves of a 14 KiB pmsg
 * tail reservation and both copies are cache-cleaned to PoC.
 *
 * Interpretation:
 *   - tick > 20 survives: generic timer IRQs still run beyond the scheduler
 *     frontier (~10 s after late_init).
 *   - all CPUs stop at tick 20/21: timer/IRQ/global CPU progress is implicated.
 *   - only one cluster stops: local CPU/cluster hotplug or interrupt state.
 */
#define A52_R341_SIDEBAND_PHYS 0xB1BFC000ULL
#define A52_R341_SIDEBAND_BYTES 0x3800U
#define A52_R341_COPY_BYTES (A52_R341_SIDEBAND_BYTES / 2U)
#define A52_R341_SLOT_BYTES 64U
#define A52_R341_SLOTS_PER_COPY (A52_R341_COPY_BYTES / A52_R341_SLOT_BYTES)
#define A52_R341_MAGIC 0x3134335244353241ULL
#define A52_R341_COMMIT 0x341c0de5U
#define A52_R341_INTERVAL_MS 10U
#define A52_R341_LIMIT 120U

enum a52_r341_event {
	A52_R341_EVT_ARM = 1,
	A52_R341_EVT_CPU_START = 2,
	A52_R341_EVT_HRTIMER = 3,
};

struct a52_r341_slot {
	u64 magic;
	u64 monotonic_ns;
	u64 sequence;
	u64 jiffies64;
	u64 reserved64;
	u32 version;
	u32 index;
	u32 event;
	u32 cpu;
	u32 tick;
	u32 commit;
};

struct a52_r341_cpu_timer {
	struct hrtimer timer;
	u32 ticks;
	u32 cpu;
};

static void *a52_r341_sideband;
static atomic_t a52_r341_sideband_index = ATOMIC_INIT(0);
static DEFINE_PER_CPU(struct a52_r341_cpu_timer, a52_r341_timers);

static void a52_r341_sideband_write(u32 event, u32 cpu, u32 tick)
{
	struct a52_r341_slot slot;
	unsigned int index;
	unsigned int pos;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r341_sideband))
		return;

	memset(&slot, 0, sizeof(slot));
	index = (unsigned int)atomic_inc_return(&a52_r341_sideband_index) - 1U;
	slot.magic = A52_R341_MAGIC;
	slot.monotonic_ns = ktime_get_ns();
	slot.sequence = (u64)atomic64_read(&a52_r179_sequence);
	slot.jiffies64 = get_jiffies_64();
	slot.reserved64 = ((u64)(u32)current->pid << 32) |
		((u64)(u32)preempt_count() << 1) |
		(irqs_disabled() ? 1ULL : 0ULL);
	slot.version = 1U;
	slot.index = index;
	slot.event = event;
	slot.cpu = cpu;
	slot.tick = tick;
	slot.commit = A52_R341_COMMIT;

	pos = (index % A52_R341_SLOTS_PER_COPY) * A52_R341_SLOT_BYTES;
	dst0 = (u8 *)a52_r341_sideband + pos;
	dst1 = (u8 *)a52_r341_sideband + A52_R341_COPY_BYTES + pos;

	memcpy(dst0, &slot, sizeof(slot));
	memcpy(dst1, &slot, sizeof(slot));
	wmb();
	__flush_dcache_area(dst0, sizeof(slot));
	__flush_dcache_area(dst1, sizeof(slot));
}

static enum hrtimer_restart a52_r341_hrtimer_fn(struct hrtimer *timer)
{
	struct a52_r341_cpu_timer *ct =
		container_of(timer, struct a52_r341_cpu_timer, timer);

	ct->ticks++;
	a52_r341_sideband_write(A52_R341_EVT_HRTIMER,
		(u32)smp_processor_id(), ct->ticks);

	if (ct->ticks >= A52_R341_LIMIT)
		return HRTIMER_NORESTART;

	hrtimer_forward_now(timer, ms_to_ktime(A52_R341_INTERVAL_MS));
	return HRTIMER_RESTART;
}

static void a52_r341_start_cpu(void *unused)
{
	struct a52_r341_cpu_timer *ct = this_cpu_ptr(&a52_r341_timers);
	unsigned int cpu = (unsigned int)smp_processor_id();

	memset(ct, 0, sizeof(*ct));
	ct->cpu = cpu;
	hrtimer_init(&ct->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	ct->timer.function = a52_r341_hrtimer_fn;
	a52_r341_sideband_write(A52_R341_EVT_CPU_START, cpu, 0U);
	hrtimer_start(&ct->timer, ms_to_ktime(A52_R341_INTERVAL_MS),
		      HRTIMER_MODE_REL_PINNED);
}

static void __used a52_r341_start(void)
{
	static const unsigned int targets[] = { 0U, 5U, 7U };
	unsigned int mask = 0;
	unsigned int i;
	int rc;

	BUILD_BUG_ON(sizeof(struct a52_r341_slot) != A52_R341_SLOT_BYTES);
	BUILD_BUG_ON(A52_R341_COPY_BYTES % A52_R341_SLOT_BYTES);

	a52_r341_sideband = memremap(A52_R341_SIDEBAND_PHYS,
		A52_R341_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r341_sideband) {
		a52_ackfr_record("P276 341A map=0 mask=0");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(targets); i++) {
		unsigned int cpu = targets[i];

		if (cpu >= nr_cpu_ids || !cpu_online(cpu))
			continue;
		rc = smp_call_function_single(cpu, a52_r341_start_cpu, NULL, 1);
		if (!rc)
			mask |= BIT(cpu);
	}

	a52_r341_sideband_write(A52_R341_EVT_ARM,
		(u32)smp_processor_id(), mask);
	a52_ackfr_record("P276 341A map=1 mask=%x int=%u lim=%u",
		mask, A52_R341_INTERVAL_MS, A52_R341_LIMIT);
}

/* A52_PHASE393_IRQ_RPMH_SMMU_CLIFF_V1
 *
 * Phase392 proved the independent scheduler heartbeat survives through
 * 18.344091 s but misses its next 500 ms frontier target (~18.406 s).
 * Arm the already-existing Phase341 raw hardirq sideband only after loop49
 * triggers. CPU0/5/7 pinned hrtimers fire every 10 ms for up to 1.2 s.
 *
 * This raw sideband is independent of kthreads/system_wq/R48 and lives at
 * 0xB1BFC000..0xB1BFF7FF. If these pulses stop at the same instant as P385,
 * timer/IRQ/global AP progress is implicated. If they continue, the problem is
 * above hardirq level (scheduler/task/locking).
 */
static struct task_struct *a52_r393_irq_arm_task;

static int a52_r393_irq_arm_fn(void *unused)
{
	u64 trigger;

	(void)unused;
	while (!kthread_should_stop()) {
		trigger = a52_ackfr_frontier_trigger_ns();
		if (trigger)
			break;
		if (msleep_interruptible(1) && kthread_should_stop())
			return 0;
	}

	if (kthread_should_stop())
		return 0;

	a52_ackfr_record("P393 IRQARM t=%llu int=%u lim=%u",
		(unsigned long long)trigger,
		A52_R341_INTERVAL_MS, A52_R341_LIMIT);
	a52_r341_start();
	return 0;
}

static int __init __maybe_unused a52_r393_irq_arm_init(void)
{
	a52_r393_irq_arm_task =
		kthread_run(a52_r393_irq_arm_fn, NULL, "a52_r393_irq");
	if (IS_ERR(a52_r393_irq_arm_task)) {
		a52_ackfr_record("P393 IRQARM err=%ld",
			PTR_ERR(a52_r393_irq_arm_task));
		a52_r393_irq_arm_task = NULL;
	}
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired late_initcall(a52_r393_irq_arm_init); */

/* A52_PHASE394_EARLY_FIXED_LANE_V1
 * Six non-wrapping process-context snapshots live in otherwise-unused bytes of
 * the Phase392 4 KiB metadata header.  Circular persistent records cannot
 * overwrite them.  They answer whether normal scheduled work still runs at
 * 15/20/25/30/35/40 s on boots that later reset with a TZ/NoC watchdog reason.
 */
#define A52_P394_EARLY_OFFSET 512U
#define A52_P394_EARLY_SLOTS 6U
#define A52_P394_EARLY_BYTES 128U
#define A52_P394_EARLY_MAGIC 0x34393345524c5931ULL
#define A52_P394_EARLY_COMMIT 0x394c0de5U

struct a52_p394_early_slot {
	u64 magic;
	u64 ts_ns;
	u64 r48_seq;
	u64 jiffies64;
	u64 frontier_ns;
	u32 target_ms;
	u32 cpu;
	u32 irq_sideband_index;
	u32 retained;
	u64 persistent_seq;
	u64 persistent_dropped;
	char comm[TASK_COMM_LEN];
	u32 crc32c;
	u32 commit;
	u8 reserved[32];
} __packed;

static const u32 a52_p394_early_targets[A52_P394_EARLY_SLOTS] = {
	15000U, 20000U, 25000U, 30000U, 35000U, 40000U,
};
static struct delayed_work a52_p394_early_work;
static unsigned int a52_p394_early_index;

static void a52_p394_early_schedule(void)
{
	u64 now_ms;
	u64 delay_ms;

	if (a52_p394_early_index >= A52_P394_EARLY_SLOTS)
		return;
	now_ms = div_u64(ktime_get_boottime_ns(), NSEC_PER_MSEC);
	if (now_ms >= a52_p394_early_targets[a52_p394_early_index])
		delay_ms = 1;
	else
		delay_ms = a52_p394_early_targets[a52_p394_early_index] - now_ms;
	schedule_delayed_work(&a52_p394_early_work,
		msecs_to_jiffies((unsigned int)delay_ms));
}

static void a52_p394_early_workfn(struct work_struct *work)
{
	struct a52_p394_early_slot slot;
	unsigned int index = a52_p394_early_index;
	int cpu;

	(void)work;
	if (index >= A52_P394_EARLY_SLOTS)
		return;

	memset(&slot, 0, sizeof(slot));
	slot.magic = A52_P394_EARLY_MAGIC;
	slot.ts_ns = ktime_get_boottime_ns();
	slot.r48_seq = (u64)atomic64_read(&a52_r179_sequence);
	slot.jiffies64 = get_jiffies_64();
	slot.frontier_ns = a52_ackfr_frontier_trigger_ns();
	slot.target_ms = a52_p394_early_targets[index];
	cpu = get_cpu();
	slot.cpu = (u32)cpu;
	put_cpu();
	slot.irq_sideband_index = (u32)atomic_read(&a52_r341_sideband_index);
	slot.retained = (u32)atomic_read(&a52_r280_retained);
	slot.persistent_seq = READ_ONCE(a52_p392_seq);
	slot.persistent_dropped = (u64)atomic64_read(&a52_p392_dropped);
	get_task_comm(slot.comm, current);
	slot.crc32c = a52_p392_crc32c(&slot,
		offsetof(struct a52_p394_early_slot, crc32c));
	slot.commit = A52_P394_EARLY_COMMIT;

	BUILD_BUG_ON(sizeof(struct a52_p394_early_slot) != A52_P394_EARLY_BYTES);
	BUILD_BUG_ON(A52_P394_EARLY_OFFSET +
		A52_P394_EARLY_SLOTS * A52_P394_EARLY_BYTES > A52_P392_HEADER_BYTES);
	if (READ_ONCE(a52_p392_base)) {
		memcpy_toio((u8 __iomem *)a52_p392_base + A52_P394_EARLY_OFFSET +
			index * A52_P394_EARLY_BYTES, &slot, sizeof(slot));
		wmb();
	}

	a52_p394_early_index++;
	a52_p394_early_schedule();
}

static int __init __maybe_unused a52_p394_early_init(void)
{
	INIT_DELAYED_WORK(&a52_p394_early_work, a52_p394_early_workfn);
	a52_p394_early_index = 0;
	a52_p394_early_schedule();
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired late_initcall(a52_p394_early_init); */



#include <uapi/linux/sched/types.h>

/* A52_PHASE342_BUSYLOOP_COUNTER_FRONTIER_V1
 *
 * Keep one CPU continuously executing across the repeatable ~12 s frontier.
 * The thread is pinned to CPU5, promoted to a low RT FIFO priority and never
 * sleeps after startup.  It polls ktime_get_ns() directly and writes a mirrored
 * raw sideband every 250 ms.  This makes each checkpoint independent of
 * hrtimer delivery, scheduler ticks, system_wq and the R48 backend.
 *
 * If ktime checkpoints continue after Phase341 hrtimers stop, CPU execution and
 * the architectural clocksource survive while interrupt/timer delivery fails.
 * If this stream also stops at the same instant, CPU/global execution itself is
 * collapsing.  If keeping CPU5 busy prevents the collapse, cpuidle/power-entry
 * becomes the leading suspect.
 */
#define A52_R342_SIDEBAND_PHYS 0xB1BFA000ULL
#define A52_R342_SIDEBAND_BYTES 0x2000U
#define A52_R342_COPY_BYTES (A52_R342_SIDEBAND_BYTES / 2U)
#define A52_R342_SLOT_BYTES 64U
#define A52_R342_SLOTS_PER_COPY (A52_R342_COPY_BYTES / A52_R342_SLOT_BYTES)
#define A52_R342_MAGIC 0x3234335244353241ULL
#define A52_R342_COMMIT 0x342c0de5U
#define A52_R342_INTERVAL_NS 250000000ULL
#define A52_R342_LIMIT 60U
#define A52_R342_CPU 5U

enum a52_r342_event {
	A52_R342_EVT_ARM = 1,
	A52_R342_EVT_THREAD_START = 2,
	A52_R342_EVT_POLL = 3,
	A52_R342_EVT_DONE = 4,
};

struct a52_r342_slot {
	u64 magic;
	u64 monotonic_ns;
	u64 sequence;
	u64 jiffies64;
	u32 version;
	u32 index;
	u32 event;
	u32 cpu;
	u32 tick;
	u32 irqs_off;
	u32 preempt;
	u32 commit;
};

static void *a52_r342_sideband;
static atomic_t a52_r342_sideband_index = ATOMIC_INIT(0);
static struct task_struct *a52_r342_task;

static void a52_r342_sideband_write(u32 event, u32 tick)
{
	struct a52_r342_slot slot;
	unsigned int index;
	unsigned int pos;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r342_sideband))
		return;

	memset(&slot, 0, sizeof(slot));
	index = (unsigned int)atomic_inc_return(&a52_r342_sideband_index) - 1U;
	slot.magic = A52_R342_MAGIC;
	slot.monotonic_ns = ktime_get_ns();
	slot.sequence = (u64)atomic64_read(&a52_r179_sequence);
	slot.jiffies64 = get_jiffies_64();
	slot.version = 1U;
	slot.index = index;
	slot.event = event;
	slot.cpu = (u32)raw_smp_processor_id();
	slot.tick = tick;
	slot.irqs_off = irqs_disabled() ? 1U : 0U;
	slot.preempt = (u32)preempt_count();
	slot.commit = A52_R342_COMMIT;

	pos = (index % A52_R342_SLOTS_PER_COPY) * A52_R342_SLOT_BYTES;
	dst0 = (u8 *)a52_r342_sideband + pos;
	dst1 = (u8 *)a52_r342_sideband + A52_R342_COPY_BYTES + pos;
	memcpy(dst0, &slot, sizeof(slot));
	memcpy(dst1, &slot, sizeof(slot));
	wmb();
	__flush_dcache_area(dst0, sizeof(slot));
	__flush_dcache_area(dst1, sizeof(slot));
}

static int a52_r342_thread_fn(void *unused)
{
	struct sched_param sp = { .sched_priority = 1 };
	u64 next;
	unsigned int tick = 0;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sp);
	a52_r342_sideband_write(A52_R342_EVT_THREAD_START, 0U);
	next = ktime_get_ns() + A52_R342_INTERVAL_NS;

	while (!kthread_should_stop() && tick < A52_R342_LIMIT) {
		u64 now = ktime_get_ns();

		if ((s64)(now - next) >= 0) {
			tick++;
			a52_r342_sideband_write(A52_R342_EVT_POLL, tick);
			next += A52_R342_INTERVAL_NS;
		}
		cpu_relax();
	}

	a52_r342_sideband_write(A52_R342_EVT_DONE, tick);
	return 0;
}

static void __used a52_r342_start(void)
{
	long kt_rc = 0;

	BUILD_BUG_ON(sizeof(struct a52_r342_slot) != A52_R342_SLOT_BYTES);
	BUILD_BUG_ON(A52_R342_COPY_BYTES % A52_R342_SLOT_BYTES);

	a52_r342_sideband = memremap(A52_R342_SIDEBAND_PHYS,
		A52_R342_SIDEBAND_BYTES, MEMREMAP_WB);
	a52_r342_task = kthread_create(a52_r342_thread_fn, NULL, "a52-p342");
	if (IS_ERR(a52_r342_task)) {
		kt_rc = PTR_ERR(a52_r342_task);
		a52_r342_task = NULL;
	} else {
		if (A52_R342_CPU < nr_cpu_ids && cpu_online(A52_R342_CPU))
			kthread_bind(a52_r342_task, A52_R342_CPU);
		wake_up_process(a52_r342_task);
	}

	a52_r342_sideband_write(A52_R342_EVT_ARM, 0U);
	a52_ackfr_record("P276 342A map=%u kt=%ld cpu=%u int=250 lim=%u",
		a52_r342_sideband ? 1U : 0U, kt_rc,
		A52_R342_CPU, A52_R342_LIMIT);
}

#include <asm/sysreg.h>

/* A52_PHASE343_INSTRUCTION_COUNTER_FRONTIER_V1
 *
 * Resolve the remaining Phase342 ambiguity without using any clock as the
 * progress condition.  CPU5 executes a tight SCHED_FIFO loop.  Progress is
 * measured only by a monotonically increasing software iteration counter.
 * Every 2^28 iterations a mirrored cache-cleaned raw sideband checkpoint is
 * written.  ktime, jiffies and CNTVCT are sampled only as payload data after
 * the iteration threshold has already been reached.
 *
 * Interpretation:
 *   - iteration checkpoints stop near the Phase342 frontier:
 *       CPU/global execution really stopped (or CPU5 was forcibly removed).
 *   - iterations continue while ktime and CNTVCT stop:
 *       architectural/kernel clock progression stopped, CPU execution lived.
 *   - iterations and CNTVCT continue while ktime/jiffies stop:
 *       kernel timekeeping/tick progression failed, not CPU execution.
 *
 * No display, clock, regulator, reset, PM, IRQ or timer behavior is changed.
 */
#define A52_R343_SIDEBAND_PHYS 0xB1BF8000ULL
#define A52_R343_SIDEBAND_BYTES 0x2000U
#define A52_R343_COPY_BYTES (A52_R343_SIDEBAND_BYTES / 2U)
#define A52_R343_SLOT_BYTES 64U
#define A52_R343_SLOTS_PER_COPY (A52_R343_COPY_BYTES / A52_R343_SLOT_BYTES)
#define A52_R343_MAGIC 0x3334335244353241ULL
#define A52_R343_COMMIT 0x343c0de5U
#define A52_R343_STRIDE_SHIFT 28U
#define A52_R343_STRIDE (1ULL << A52_R343_STRIDE_SHIFT)
#define A52_R343_LIMIT 128U
#define A52_R343_CPU 5U

enum a52_r343_event {
	A52_R343_EVT_ARM = 1,
	A52_R343_EVT_THREAD_START = 2,
	A52_R343_EVT_ITERATION = 3,
	A52_R343_EVT_DONE = 4,
};

struct a52_r343_slot {
	u64 magic;
	u64 iteration;
	u64 monotonic_ns;
	u64 cntvct;
	u64 jiffies64;
	u32 index;
	u32 event;
	u32 cpu;
	u32 checkpoint;
	u32 preempt;
	u32 commit;
};


/* A52_PHASE357_INIT_EXIT_CAUSE_V1
 *
 * Phase355/356 resolved the apparent ~12 s system freeze: CPU6 is executing
 * Linux panic()'s final mdelay(PANIC_TIMER_STEP) loop in PID1/init context.
 * Phase356's saved LR resolves exactly to panic+0x300, immediately after the
 * __const_udelay(1000 * 0x10c7) call used by mdelay(100).
 *
 * Phase357 records how PID1 reached do_exit and the exact values used by the
 * global-init panic branch. The final state is replicated across the whole
 * 32 KiB sideband so pmsg clear-bit damage cannot erase the diagnosis.
 */
#define A52_R357_SIDEBAND_PHYS   0xB1BF0000ULL
#define A52_R357_SIDEBAND_BYTES  0x8000U
#define A52_R357_COPY_BYTES      0x4000U
#define A52_R357_SLOT_BYTES      256U
#define A52_R357_SLOTS_PER_COPY  64U
#define A52_R357_MAGIC           0x3735335449584549ULL /* "IEXIT357" */
#define A52_R357_COMMIT          0x357c0de5U

#define A52_R357_N_GROUP_EXIT    BIT(0)
#define A52_R357_N_DO_EXIT       BIT(1)
#define A52_R357_N_MAKE_DEAD     BIT(2)
#define A52_R357_N_INIT_PANIC    BIT(3)

struct a52_r357_record {
	u64 magic;
	u64 ns;
	u64 sequence;
	u64 runtime_text;
	u64 do_exit_code;
	u64 panic_code;
	u64 jobctl;
	u64 mm;
	u64 task_flags;
	u64 reserved64;
	u32 note_mask;
	u32 cpu;
	u32 pid;
	u32 tgid;
	s32 group_exit_code;
	s32 task_exit_code;
	s32 do_group_exit_code;
	s32 make_task_dead_signr;
	u32 signal_flags;
	s32 exit_signal;
	u32 pending_signal;
	u32 fatal_signal;
	s32 signal_live;
	u32 commit;
	u32 version;
	char comm[TASK_COMM_LEN];
	u8 reserved[96];
};

extern char _text[];
static void *a52_r357_sideband;
static u32 a52_r357_note_mask;
static s64 a52_r357_do_exit_code;
static s32 a52_r357_group_exit_code;
static s32 a52_r357_make_dead_signr;

void a52_p357_note_group_exit(int exit_code)
{
	if (current->pid != 1)
		return;
	WRITE_ONCE(a52_r357_group_exit_code, (s32)exit_code);
	WRITE_ONCE(a52_r357_note_mask,
		READ_ONCE(a52_r357_note_mask) | A52_R357_N_GROUP_EXIT);
}

void a52_p357_note_do_exit(long code)
{
	if (current->pid != 1)
		return;
	WRITE_ONCE(a52_r357_do_exit_code, (s64)code);
	WRITE_ONCE(a52_r357_note_mask,
		READ_ONCE(a52_r357_note_mask) | A52_R357_N_DO_EXIT);
}

void a52_p357_note_make_task_dead(int signr)
{
	if (current->pid != 1)
		return;
	WRITE_ONCE(a52_r357_make_dead_signr, (s32)signr);
	WRITE_ONCE(a52_r357_note_mask,
		READ_ONCE(a52_r357_note_mask) | A52_R357_N_MAKE_DEAD);
}

void a52_p357_persist_init_panic(long code, int group_exit_code)
{
	struct a52_r357_record r;
	unsigned int i;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r357_sideband) || current->pid != 1)
		return;

	BUILD_BUG_ON(sizeof(struct a52_r357_record) != A52_R357_SLOT_BYTES);
	memset(&r, 0, sizeof(r));
	r.magic = A52_R357_MAGIC;
	r.ns = ktime_get_ns();
	r.sequence = (u64)atomic64_read(&a52_r179_sequence);
	r.runtime_text = (u64)(unsigned long)_text;
	r.do_exit_code = (u64)READ_ONCE(a52_r357_do_exit_code);
	r.panic_code = (u64)(s64)code;
	r.jobctl = (u64)READ_ONCE(current->jobctl);
	r.mm = (u64)(unsigned long)READ_ONCE(current->mm);
	r.task_flags = (u64)READ_ONCE(current->flags);
	r.note_mask = READ_ONCE(a52_r357_note_mask) | A52_R357_N_INIT_PANIC;
	r.cpu = (u32)raw_smp_processor_id();
	r.pid = (u32)current->pid;
	r.tgid = (u32)current->tgid;
	r.group_exit_code = (s32)group_exit_code;
	r.task_exit_code = (s32)READ_ONCE(current->exit_code);
	r.do_group_exit_code = READ_ONCE(a52_r357_group_exit_code);
	r.make_task_dead_signr = READ_ONCE(a52_r357_make_dead_signr);
	r.signal_flags = READ_ONCE(current->signal->flags);
	r.exit_signal = (s32)READ_ONCE(current->exit_signal);
	r.pending_signal = signal_pending(current) ? 1U : 0U;
	r.fatal_signal = fatal_signal_pending(current) ? 1U : 0U;
	r.signal_live = (s32)atomic_read(&current->signal->live);
	r.commit = A52_R357_COMMIT;
	r.version = 1U;
	memcpy(r.comm, current->comm, TASK_COMM_LEN);

	for (i = 0; i < A52_R357_SLOTS_PER_COPY; i++) {
		dst0 = (u8 *)a52_r357_sideband + i * A52_R357_SLOT_BYTES;
		dst1 = (u8 *)a52_r357_sideband + A52_R357_COPY_BYTES +
			i * A52_R357_SLOT_BYTES;
		memcpy(dst0, &r, sizeof(r));
		memcpy(dst1, &r, sizeof(r));
	}
	wmb();
	__flush_dcache_area(a52_r357_sideband, A52_R357_SIDEBAND_BYTES);
}

static void __used a52_r357_start(void)
{
	BUILD_BUG_ON(sizeof(struct a52_r357_record) != A52_R357_SLOT_BYTES);
	BUILD_BUG_ON(A52_R357_SLOTS_PER_COPY * A52_R357_SLOT_BYTES !=
		A52_R357_COPY_BYTES);

	a52_r357_sideband = memremap(A52_R357_SIDEBAND_PHYS,
		A52_R357_SIDEBAND_BYTES, MEMREMAP_WB);
	if (!a52_r357_sideband) {
		a52_ackfr_record("P276 357A map=0");
		return;
	}

	memset(a52_r357_sideband, 0, A52_R357_SIDEBAND_BYTES);
	a52_r357_note_mask = 0U;
	a52_r357_do_exit_code = 0;
	a52_r357_group_exit_code = 0;
	a52_r357_make_dead_signr = 0;
	wmb();
	__flush_dcache_area(a52_r357_sideband, A52_R357_SIDEBAND_BYTES);
	a52_ackfr_record("P276 357A map=1");
}

static void *a52_r343_sideband;
static atomic_t a52_r343_sideband_index = ATOMIC_INIT(0);
static struct task_struct *a52_r343_task;

static void a52_r343_sideband_write(u32 event, u32 checkpoint, u64 iteration)
{
	struct a52_r343_slot slot;
	unsigned int index;
	unsigned int pos;
	void *dst0;
	void *dst1;

	if (!READ_ONCE(a52_r343_sideband))
		return;

	memset(&slot, 0, sizeof(slot));
	index = (unsigned int)atomic_inc_return(&a52_r343_sideband_index) - 1U;
	slot.magic = A52_R343_MAGIC;
	slot.iteration = iteration;
	slot.monotonic_ns = ktime_get_ns();
	slot.cntvct = read_sysreg(cntvct_el0);
	slot.jiffies64 = get_jiffies_64();
	slot.index = index;
	slot.event = event;
	slot.cpu = (u32)raw_smp_processor_id();
	slot.checkpoint = checkpoint;
	slot.preempt = (u32)preempt_count();
	slot.commit = A52_R343_COMMIT;

	pos = (index % A52_R343_SLOTS_PER_COPY) * A52_R343_SLOT_BYTES;
	dst0 = (u8 *)a52_r343_sideband + pos;
	dst1 = (u8 *)a52_r343_sideband + A52_R343_COPY_BYTES + pos;
	memcpy(dst0, &slot, sizeof(slot));
	memcpy(dst1, &slot, sizeof(slot));
	wmb();
	__flush_dcache_area(dst0, sizeof(slot));
	__flush_dcache_area(dst1, sizeof(slot));
}

static int a52_r343_thread_fn(void *unused)
{
	struct sched_param sp = { .sched_priority = 1 };
	u64 iteration = 0;
	u64 next = A52_R343_STRIDE;
	unsigned int checkpoint = 0;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sp);
	a52_r343_sideband_write(A52_R343_EVT_THREAD_START, 0U, 0ULL);

	while (!kthread_should_stop() && checkpoint < A52_R343_LIMIT) {
		iteration++;

		if (unlikely(iteration == next)) {
			checkpoint++;
			a52_r343_sideband_write(A52_R343_EVT_ITERATION,
				checkpoint, iteration);
			next += A52_R343_STRIDE;
		}
		cpu_relax();
	}

	a52_r343_sideband_write(A52_R343_EVT_DONE, checkpoint, iteration);
	return 0;
}

static void __used a52_r343_start(void)
{
	long kt_rc = 0;

	BUILD_BUG_ON(sizeof(struct a52_r343_slot) != A52_R343_SLOT_BYTES);
	BUILD_BUG_ON(A52_R343_COPY_BYTES % A52_R343_SLOT_BYTES);

	a52_r343_sideband = memremap(A52_R343_SIDEBAND_PHYS,
		A52_R343_SIDEBAND_BYTES, MEMREMAP_WB);
	a52_r343_task = kthread_create(a52_r343_thread_fn, NULL, "a52-p343");
	if (IS_ERR(a52_r343_task)) {
		kt_rc = PTR_ERR(a52_r343_task);
		a52_r343_task = NULL;
	} else {
		if (A52_R343_CPU < nr_cpu_ids && cpu_online(A52_R343_CPU))
			kthread_bind(a52_r343_task, A52_R343_CPU);
		wake_up_process(a52_r343_task);
	}

	a52_r343_sideband_write(A52_R343_EVT_ARM, 0U, 0ULL);
	a52_ackfr_record("P276 343A map=%u kt=%ld cpu=%u sh=%u lim=%u",
		a52_r343_sideband ? 1U : 0U, kt_rc,
		A52_R343_CPU, A52_R343_STRIDE_SHIFT, A52_R343_LIMIT);
}

static unsigned int a52_r339_checkpoint_index;
static void a52_r339_preservation_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(a52_r339_preservation_work,
			    a52_r339_preservation_fn);

static void a52_r339_preservation_fn(struct work_struct *work)
{
	unsigned int index = a52_r339_checkpoint_index;
	unsigned int now_s;
	unsigned int next_s;
	u64 seq;
	unsigned int retained;

	if (index >= ARRAY_SIZE(a52_r339_checkpoints_s))
		return;

	now_s = a52_r339_checkpoints_s[index];
	seq = (u64)atomic64_read(&a52_r179_sequence);
	retained = (unsigned int)atomic_read(&a52_r280_retained);

	if (now_s == 330U)
		a52_ackfr_record("P276 339R t=%u q=%llu r=%u warm=1",
			now_s, (unsigned long long)seq, retained);
	else
		a52_ackfr_record("P276 339S t=%u q=%llu r=%u j=%lu",
			now_s, (unsigned long long)seq, retained, jiffies);

	/* Order the persistent transport writes before any following checkpoint
	 * and, at 330 s, before the restart path begins.
	 */
	wmb();

	a52_r339_checkpoint_index = index + 1;
	if (now_s == 330U) {
		pr_emerg("Phase339 warm reboot into recovery after preservation checkpoint\\n");
		msleep(250);
		kernel_restart("recovery");
		a52_ackfr_record("P276 339E reboot_returned=1");
		wmb();
		return;
	}

	next_s = a52_r339_checkpoints_s[index + 1];
	schedule_delayed_work(&a52_r339_preservation_work,
		msecs_to_jiffies((next_s - now_s) * 1000U));
}

static atomic_t a52_r179_heartbeat_count = ATOMIC_INIT(0);
static void a52_r179_heartbeat_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(a52_r179_heartbeat_work,
			    a52_r179_heartbeat_fn);

static void a52_r179_heartbeat_fn(struct work_struct *work)
{
	unsigned int tick;

	tick = (unsigned int)atomic_inc_return(&a52_r179_heartbeat_count);
	a52_r230_replay_journal(tick);
	a52_r226_task_snapshot(tick);
	a52_r228_tripost_snapshot(tick);
	a52_r267_display_snapshot(tick);
	a52_r268_snapshot(tick);
	/* A52_PHASE243_PHASE242_RUNTIME_DISABLED_V1: no Phase 242 heartbeat snapshot */
	a52_ackfr_record("HB tick=%u online=%u run=%lu j=%lu", tick,
			  num_online_cpus(), nr_running(), jiffies);
	/* A52_PHASE242_PHASE241_REPLAY_DISABLED_V1
	 * Phase 241 hardware showed the bulk replay path itself is unreliable.
	 * Keep source-side latching, but do not execute the bucket replay.
	 */
	if (tick < A52_R179_HEARTBEAT_LIMIT)
		schedule_delayed_work(&a52_r179_heartbeat_work,
			msecs_to_jiffies(A52_R179_HEARTBEAT_INTERVAL_MS));
}

static int __init a52_r179_early_heartbeat(void)
{
	a52_ackfr_record("BOOT phase=pre_smp");
	return 0;
}
early_initcall(a52_r179_early_heartbeat);

static int __init __maybe_unused a52_r179_pure_heartbeat(void)
{
	a52_ackfr_record("BOOT phase=pure");
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired pure_initcall(a52_r179_pure_heartbeat); */

static int __init __maybe_unused a52_r179_core_heartbeat(void)
{
	a52_ackfr_record("BOOT phase=core");
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired core_initcall(a52_r179_core_heartbeat); */

static int __init __maybe_unused a52_r179_postcore_heartbeat(void)
{
	a52_ackfr_record("BOOT phase=postcore");
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired postcore_initcall(a52_r179_postcore_heartbeat); */

static int __init __maybe_unused a52_r179_arch_heartbeat(void)
{
	a52_ackfr_record("BOOT phase=arch");
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired arch_initcall(a52_r179_arch_heartbeat); */

static int __init __maybe_unused a52_r179_subsys_heartbeat(void)
{
	a52_ackfr_record("BOOT phase=subsys");
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired subsys_initcall(a52_r179_subsys_heartbeat); */

static int __init __maybe_unused a52_r179_fs_heartbeat(void)
{
	a52_ackfr_record("BOOT phase=fs");
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired fs_initcall(a52_r179_fs_heartbeat); */

static int __init __maybe_unused a52_r179_device_retry(void)
{
	a52_ackfr_record("BOOT phase=device");
	a52_r179_flush_missing((u64)atomic64_read(&a52_r179_sequence));
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired device_initcall(a52_r179_device_retry); */

static int __init __maybe_unused a52_r179_late_retry(void)
{
	u64 stored;

	a52_ackfr_record("BOOT phase=late");
	stored = (u64)atomic64_read(&a52_r179_sequence);
	a52_r179_flush_missing(stored);
	a52_r179_write_control("BOOT_READY");
	/*
	 * A52_PHASE380_LEGACY_PROBES_RETIRED_V1
	 *
	 * The old post-display frontier probes are no longer relevant to the
	 * UFS/loop/block stall and some actively perturb this boot:
	 *  - Phase273 + heartbeat consume system_wq
	 *  - Phase339 eventually forces a recovery restart
	 *  - Phase340 owns the old 0xB1BFF800 sideband
	 *  - Phase341 pins hardirq timers into 0xB1BFC000
	 *  - Phase343 runs a SCHED_FIFO busy loop on CPU5 and owns 0xB1BF8000
	 *
	 * Preserve their source for lineage, but do not start them at runtime.
	 * Phase380 owns 0xB1BF8000..0xB1BFFFFF exclusively.
	 */
	a52_ackfr_record("P380 legacy probes retired hb=1 p273=1 p339=1 p340=1 p341=1 p343=1");
	pr_info("phase199 triple-copy RS+CRC32C recorder enabled stored=%llu dropped=%llu\n",
		(unsigned long long)min_t(u64, stored, A52_R179_CAPACITY),
		(unsigned long long)atomic64_read(&a52_r179_dropped));
	return 0;
}
/* A52_PHASE402_DSI_NOHLT_CLEAN_V1: retired late_initcall(a52_r179_late_retry); */

/* A52_PHASE402_DSI_NOHLT_CLEAN_V1
 * Runtime policy: retain DSI/DMA_DONE probes + Phase392 filtered persistent trace.
 * Phase397 UFS/USB bus contracts are functional fixes, not probes.
 */
static const char a52_p402_marker[] __used = "A52_PHASE402_DSI_NOHLT_CLEAN_V1";

/* A52_PHASE406_P392_CACHE_PERSIST_V1
 * Phase392 uses ioremap_cache(); explicit dcache cleaning is required
 * for reserved-RAM evidence to survive reset into recovery.
 */
static const char a52_p406_marker[] __used = "A52_PHASE406_P392_CACHE_PERSIST_V1";

/* A52_PHASE415_MDSS_GDSC_HWCTRL_PARITY_V1: P415 GDSC events admitted to 3 MiB recorder only. */

/* A52_PHASE416_SEQUENTIAL_3M_DISK_PERSIST_V2: late-armed Samsung transport with persistent status sidecar. */

/* A52_PHASE417_POSTBIND_FRONTIER_V1: post-bind task/DRM bridge frontier only. */

/* A52_PHASE418_ATOMIC_PRESENT_DISCRIMINATOR_V1: sparse atomic-present + APEX frontier. */

/* A52_PHASE419_UMODE_VREFRESH_PARITY_V1: P419 one-shot evidence admitted to sequential disk tier. */

/* A52_PHASE420_UMODE_ROUNDTRIP_VREFRESH_PARITY_V1: P420 round-trip evidence admitted to sequential disk tier. */


/* A52_PHASE421_FETCH_MEMORY_SURVIVAL_V1
 *
 * Corruption-tolerant fixed-slot survival lane for the first real F0 5A 5A
 * panel transaction. CRC/commit are convenience checks only. The raw repeated
 * fields are intentionally decodable even when every CRC fails after a manual
 * reset.
 *
 * copy0: B1900000 + 0x500 (11 x 128)
 * copy1: B1900000 + 0xA80 (11 x 128)
 * copy2: B1A00000 + 0x800 (11 x 128), exactly 1 MiB from copy0/1 page
 *
 * The B1A location is in Phase414's reserved 4 KiB header page. Phase414 data
 * records begin at +0x1000; Phase416 status copies end below +0x280.
 *
 * Slot layout is exactly 128 bytes:
 *   stage_a[32], flags[5], rc[5], nonce[5],
 *   stage_b[32], irq[5], ret[5], valid[5], format, crc32c, commit.
 *
 * Stage is encoded as 0xD0 + stage so an unwritten zero-cleared slot cannot
 * masquerade as S00. Every later stage carries forward all previously learned
 * small values. The nonce is random per boot/first target and repeated 5x.
 */
#define A52_P421_OFF0              0x00000500U
#define A52_P421_OFF1              0x00000A80U
#define A52_P421_OFF2              0x00000800U
#define A52_P421_SLOT_BYTES        128U
#define A52_P421_SLOTS             11U
#define A52_P421_STAGE_BASE        0xD0U
#define A52_P421_FORMAT            0xA1U
#define A52_P421_COMMIT            0x421c0de5U
#define A52_P421_V_FLAGS           BIT(0)
#define A52_P421_V_RC              BIT(1)
#define A52_P421_V_IRQ             BIT(2)
#define A52_P421_V_RET             BIT(3)

struct a52_p421_slot {
	u8 stage_a[32];
	u8 flags[5];
	s16 rc[5];
	u32 nonce[5];
	u8 stage_b[32];
	u8 irq[5];
	s16 ret[5];
	u8 valid[5];
	u8 format;
	u32 crc32c;
	u32 commit;
} __packed;

static u32 a52_p421_nonce;
static u8 a52_p421_flags;
static s16 a52_p421_rc;
static u8 a52_p421_irq;
static s16 a52_p421_ret;
static u8 a52_p421_valid;
static atomic_t a52_p421_highest = ATOMIC_INIT(-1);

static u32 a52_p421_get_nonce(void)
{
	u32 old;
	u32 nonce;

	old = READ_ONCE(a52_p421_nonce);
	if (old)
		return old;

	nonce = get_random_u32();
	if (!nonce)
		nonce = 0x42142101U;
	old = cmpxchg(&a52_p421_nonce, 0U, nonce);
	return old ? old : nonce;
}

static void a52_p421_copy_persist(void __iomem *dst,
				  const struct a52_p421_slot *slot)
{
	if (!dst || !slot)
		return;

	memcpy_toio(dst, slot, sizeof(*slot));
	/* Order the copy, clean the cached reserved mapping, then do not allow the
	 * caller to proceed into the next DSI operation until persistence reaches
	 * the architectural completion point.
	 */
	wmb();
	__flush_dcache_area((void __force *)dst, sizeof(*slot));
	dsb(sy);
}


/* A52_PHASE422_DISPLAY_SMMU_FAULT_PROBE_V1
 *
 * Four corruption-tolerant 64-byte records, each written three times:
 *   copy0: B1900000 + 0x200
 *   copy1: B1900000 + 0x300
 *   copy2: B1A00000 + 0xD80 (1 MiB-separated surviving page)
 *
 * M00: cached SID 0x800 context-IRQ registration + exact cmd_buffer_iova
 * M01: first SID 0x800 context fault during the exact Phase421 F0 window
 * M02: post-timeout SID 0x800 FSR/FAR/SCTLR snapshot
 * M03: timestamp captured at Phase421 S08 immediately after SW_TRIGGER returns
 *
 * This lane never changes SMMU or DSI behavior. CRC is convenience evidence;
 * the three physical copies remain independently decodable after bit decay.
 */
#define A52_P422_OFF0              0x00000200U
#define A52_P422_OFF1              0x00000300U
#define A52_P422_OFF2              0x00000D80U
#define A52_P422_SLOT_BYTES        64U
#define A52_P422_SLOTS             4U
#define A52_P422_MAGIC             0xA52F4220A52F4220ULL
#define A52_P422_COMMIT            0x422c0de5U

struct a52_p422_slot {
	u64 magic;
	u64 ts_ns;
	u64 arg0;
	u64 arg1;
	u64 arg2;
	u64 arg3;
	u32 stage;
	u32 seq;
	u32 crc32c;
	u32 commit;
} __packed;

static atomic_t a52_p422_written[A52_P422_SLOTS] = {
	ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0)
};

static void a52_p422_copy_persist(void __iomem *dst,
				   const struct a52_p422_slot *slot)
{
	if (!dst || !slot)
		return;

	memcpy_toio(dst, slot, sizeof(*slot));
	wmb();
	__flush_dcache_area((void __force *)dst, sizeof(*slot));
	dsb(sy);
}

void a52_p422_survival_record_at(u32 stage, u32 seq, u64 ts_ns,
				 u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
	struct a52_p422_slot slot;
	void __iomem *dst;

	BUILD_BUG_ON(sizeof(struct a52_p422_slot) != A52_P422_SLOT_BYTES);
	BUILD_BUG_ON(A52_P422_OFF1 +
		     A52_P422_SLOTS * A52_P422_SLOT_BYTES >
		     A52_P421_OFF0);
	BUILD_BUG_ON(A52_P422_OFF2 +
		     A52_P422_SLOTS * A52_P422_SLOT_BYTES >
		     A52_P414_HEADER_BYTES);

	if (stage >= A52_P422_SLOTS)
		return;
	if (atomic_cmpxchg(&a52_p422_written[stage], 0, 1) != 0)
		return;

	memset(&slot, 0, sizeof(slot));
	slot.magic = A52_P422_MAGIC;
	slot.ts_ns = ts_ns;
	slot.arg0 = arg0;
	slot.arg1 = arg1;
	slot.arg2 = arg2;
	slot.arg3 = arg3;
	slot.stage = stage;
	slot.seq = seq;
	slot.crc32c = a52_p392_crc32c(&slot,
				      offsetof(struct a52_p422_slot, crc32c));
	slot.commit = A52_P422_COMMIT;

	if (READ_ONCE(a52_p392_base)) {
		dst = (u8 __iomem *)a52_p392_base + A52_P422_OFF0 +
		      stage * A52_P422_SLOT_BYTES;
		a52_p422_copy_persist(dst, &slot);
		dst = (u8 __iomem *)a52_p392_base + A52_P422_OFF1 +
		      stage * A52_P422_SLOT_BYTES;
		a52_p422_copy_persist(dst, &slot);
	}

	if (READ_ONCE(a52_p414_ram)) {
		dst = (u8 __iomem *)a52_p414_ram + A52_P422_OFF2 +
		      stage * A52_P422_SLOT_BYTES;
		a52_p422_copy_persist(dst, &slot);
	}
}
EXPORT_SYMBOL_GPL(a52_p422_survival_record_at);

void a52_p421_survival_record(u8 stage, u8 valid, u32 flags,
			      int rc, u32 irq, int ret)
{
	struct a52_p421_slot slot;
	void __iomem *dst;
	u8 code;
	u32 nonce;
	u64 a52_p422_s08_ns = 0;
	int previous;
	unsigned int i;

	BUILD_BUG_ON(sizeof(struct a52_p421_slot) != A52_P421_SLOT_BYTES);
	BUILD_BUG_ON(A52_P421_OFF1 +
		     A52_P421_SLOTS * A52_P421_SLOT_BYTES >
		     A52_P392_HEADER_BYTES);
	BUILD_BUG_ON(A52_P421_OFF2 +
		     A52_P421_SLOTS * A52_P421_SLOT_BYTES >
		     A52_P414_HEADER_BYTES);

	if (stage >= A52_P421_SLOTS)
		return;

	/* Stages are monotonic for the one exact target. Duplicate/late callbacks
	 * cannot rewrite a slot or move the frontier backwards.
	 */
	for (;;) {
		previous = atomic_read(&a52_p421_highest);
		if ((int)stage <= previous)
			return;
		if (atomic_cmpxchg(&a52_p421_highest, previous, stage) == previous)
			break;
	}

	/* Timestamp the accepted S08 call before any survival-copy latency. */
	if (stage == 8U)
		a52_p422_s08_ns = ktime_get_boottime_ns();

	nonce = a52_p421_get_nonce();
	if (valid & A52_P421_V_FLAGS)
		a52_p421_flags = (u8)flags;
	if (valid & A52_P421_V_RC)
		a52_p421_rc = (s16)rc;
	if (valid & A52_P421_V_IRQ)
		a52_p421_irq = (u8)irq;
	if (valid & A52_P421_V_RET)
		a52_p421_ret = (s16)ret;
	a52_p421_valid |= valid;

	memset(&slot, 0, sizeof(slot));
	code = (u8)(A52_P421_STAGE_BASE + stage);
	memset(slot.stage_a, code, sizeof(slot.stage_a));
	memset(slot.stage_b, code, sizeof(slot.stage_b));
	for (i = 0; i < ARRAY_SIZE(slot.flags); i++)
		slot.flags[i] = a52_p421_flags;
	for (i = 0; i < ARRAY_SIZE(slot.rc); i++)
		slot.rc[i] = a52_p421_rc;
	for (i = 0; i < ARRAY_SIZE(slot.nonce); i++)
		slot.nonce[i] = nonce;
	for (i = 0; i < ARRAY_SIZE(slot.irq); i++)
		slot.irq[i] = a52_p421_irq;
	for (i = 0; i < ARRAY_SIZE(slot.ret); i++)
		slot.ret[i] = a52_p421_ret;
	memset(slot.valid, a52_p421_valid, sizeof(slot.valid));
	slot.format = A52_P421_FORMAT;
	slot.crc32c = a52_p392_crc32c(&slot,
				      offsetof(struct a52_p421_slot, crc32c));
	slot.commit = A52_P421_COMMIT;

	/* Each physical copy is independently completed. In particular, copy0 is
	 * fully cache-cleaned + dsb before copy1 begins.
	 */
	if (READ_ONCE(a52_p392_base)) {
		dst = (u8 __iomem *)a52_p392_base + A52_P421_OFF0 +
		      (unsigned int)stage * A52_P421_SLOT_BYTES;
		a52_p421_copy_persist(dst, &slot);

		dst = (u8 __iomem *)a52_p392_base + A52_P421_OFF1 +
		      (unsigned int)stage * A52_P421_SLOT_BYTES;
		a52_p421_copy_persist(dst, &slot);
	}

	if (READ_ONCE(a52_p414_ram)) {
		dst = (u8 __iomem *)a52_p414_ram + A52_P421_OFF2 +
		      (unsigned int)stage * A52_P421_SLOT_BYTES;
		a52_p421_copy_persist(dst, &slot);
	}

	if (stage == 8U)
		a52_p422_survival_record_at(3U, 1U, a52_p422_s08_ns,
				       0, 0, 0, 0);
}
EXPORT_SYMBOL_GPL(a52_p421_survival_record);


/* A52_PHASE423_RPMH_SOLVER_MODE_COMPAT_V1: P423 admitted to sequential recorder only. */

/* A52_PHASE424_PAIRED_HW_SNAPSHOT_V1: P424 post-timeout dump admitted to sequential recorder. */

/* A52_PHASE425_REBOOT_ORIGIN_RECORDER_V1: P425 reboot-origin records admitted. */

/* A52_PHASE426_SMMU_STREAM_HANDOFF_PROBE_V1: P426 deferred SMMU stream-map evidence admitted. */

/* A52_PHASE427_CB2_HANDOFF_ROAD_PROBE_V1: P427 evidence admitted. */

/* A52_PHASE429_APEX_SF_FOCUSED_CENSUS_V1: focused APEX/SF census admitted; DSI instrumentation unchanged. */

/* A52_PHASE430_SF_DMA_FORENSICS_V1: P430 evidence admitted. */

/* A52_PHASE431_SAFE_SF_GAP_FORENSICS_V1: P431 snapshot/witness markers admitted. */

/* A52_PHASE432_SF_HWC_BOOTANIM_CHAIN_V1: real DRM-owner identity + P432 chronology admitted. */

/* A52_PHASE434_CLEAN_F0_V1: P434 event records admitted. */

/* A52_PHASE436_P414_TAIL_RESERVE_V1: final 16 KiB reserved for Phase436. */

/* A52_PHASE437_KGSL_OPEN_CORRIDOR_V1: final 24 KiB reserved from P414: P437 8 KiB + P436 16 KiB. */

/* A52_PHASE439_DSI_AUTOPSY_V3: P439 records admitted. */


/* A52_PHASE440_EARLY_F0_CLEANUP_FORENSICS_V1
 *
 * Small bounded passive event stream for the early-vs-late F0 experiment.
 * The broad 1..80 s window intentionally includes bootanimation death and
 * late cleanup. Runtime-PM/genpd events are name-filtered to avoid flooding
 * the persistent recorder.
 */
static atomic_t a52_p440_event_count = ATOMIC_INIT(0);

static bool a52_p440_power_name(const char *name)
{
	static const char * const keys[] = {
		"dsi", "mdss", "sde", "disp", "mdp", "gpu", "kgsl",
		"smmu", "tbu", "mnoc", "noc", "icc", "interconnect",
		"gdsc", "cx", "mx"
	};
	unsigned int i;

	if (!name || !*name)
		return false;
	for (i = 0; i < ARRAY_SIZE(keys); i++)
		if (strstr(name, keys[i]))
			return true;
	return false;
}

void a52_p440_event(const char *kind, const char *name, long a, long b)
{
	u64 ns = ktime_get_boottime_ns();
	u64 ms = div_u64(ns, NSEC_PER_MSEC);
	unsigned int n;

	if (ms < 1000ULL || ms > 80000ULL)
		return;

	/* Runtime power can be extremely chatty. Keep only display-adjacent names. */
	if ((!strncmp(kind, "RPM_", 4) || !strncmp(kind, "GENPD_", 6)) &&
	    !a52_p440_power_name(name))
		return;

	n = (unsigned int)atomic_inc_return(&a52_p440_event_count);
	if (n > 512U)
		return;

	a52_ackfr_record("P440 EV n=%u ms=%llu k=%.15s d=%.31s a=%ld b=%ld",
			 n, (unsigned long long)ms,
			 kind ? kind : "-", name ? name : "-", a, b);
}
EXPORT_SYMBOL_GPL(a52_p440_event);

static const char a52_p440_marker[] __used =
	"A52_PHASE440_EARLY_F0_CLEANUP_FORENSICS_V1";

/* A52_PHASE444_RESERVED_PARTITION_V2: P414 B1A00000-B1A61FFF; P444 B1A62000-B1AF9FFF; P437/P436 upper tail preserved. */

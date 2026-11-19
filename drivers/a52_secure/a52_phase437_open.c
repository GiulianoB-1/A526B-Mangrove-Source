// SPDX-License-Identifier: GPL-2.0-only
/* A52_PHASE437_KGSL_OPEN_CORRIDOR_V1
 * Independent fixed-slot recorder around the first SurfaceFlinger
 * /dev/kgsl-3d0 open. No polling, no remote-task inspection, no DSI/MMIO.
 */
#include <linux/a52_phase437_open.h>
#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <asm/cacheflush.h>
#include <asm/ptrace.h>

#define A52_P437_PHYS       0xB1AFA000ULL
#define A52_P437_BYTES      0x2000U
#define A52_P437_COPY_BYTES 0x1000U
#define A52_P437_SLOT_BYTES 128U
#define A52_P437_SLOTS      32U
#define A52_P437_MAGIC      0x3733344543415254ULL /* TRACE437 */
#define A52_P437_COMMIT     0x437c0de5U
#define A52_P437_VERSION    1U

const char a52_p437_build_tag[] = "A52_PHASE437_KGSL_OPEN_CORRIDOR_V1";

struct a52_p437_record {
 u64 magic,boot_id,boottime_ns,arch_counter,arg0,arg1,pc,lr,sp,pstate;
 u32 version,stage,cpu,pid,tgid,slot;
 char comm[TASK_COMM_LEN];
 u32 crc32c,commit;
} __packed;
static void *a52_p437_base;
static atomic_t a52_p437_written[A52_P437_SLOTS];
static atomic_t a52_p437_target_pid=ATOMIC_INIT(-1);
static atomic_t a52_p437_target_tgid=ATOMIC_INIT(-1);
static u64 a52_p437_id;
static __always_inline u64 p437_cnt(void){u64 v;asm volatile("isb; mrs %0, cntpct_el0":"=r"(v));return v;}
static __always_inline u64 p437_frq(void){u64 v;asm volatile("mrs %0, cntfrq_el0":"=r"(v));return v;}
static u32 p437_crc(const void *buffer,size_t len){const u8 *b=buffer;u32 c=~0U;size_t i;unsigned int bit;for(i=0;i<len;i++){c^=b[i];for(bit=0;bit<8;bit++)c=(c>>1)^((c&1U)?0x82f63b78U:0U);}return ~c;}
static void p437_copy(void *dst,const struct a52_p437_record *r){struct a52_p437_record *o=dst;memcpy(o,r,offsetof(struct a52_p437_record,commit));wmb();WRITE_ONCE(o->commit,A52_P437_COMMIT);wmb();__flush_dcache_area(o,sizeof(*o));dsb(sy);}
void a52_p437_mark(u32 stage,u64 arg0,u64 arg1,const struct pt_regs *regs){struct a52_p437_record r;void *d0,*d1;if(!READ_ONCE(a52_p437_base)||stage>=A52_P437_SLOTS)return;if(atomic_cmpxchg(&a52_p437_written[stage],0,1))return;memset(&r,0,sizeof(r));r.magic=A52_P437_MAGIC;r.boot_id=READ_ONCE(a52_p437_id);r.boottime_ns=ktime_get_boottime_ns();r.arch_counter=p437_cnt();r.arg0=arg0;r.arg1=arg1;if(regs){r.pc=regs->pc;r.lr=regs->regs[30];r.sp=regs->sp;r.pstate=regs->pstate;}r.version=A52_P437_VERSION;r.stage=stage;r.cpu=raw_smp_processor_id();r.pid=current->pid;r.tgid=current->tgid;r.slot=stage;memcpy(r.comm,current->comm,TASK_COMM_LEN);r.crc32c=p437_crc(&r,offsetof(struct a52_p437_record,crc32c));d0=(u8*)a52_p437_base+stage*A52_P437_SLOT_BYTES;d1=(u8*)a52_p437_base+A52_P437_COPY_BYTES+stage*A52_P437_SLOT_BYTES;p437_copy(d0,&r);p437_copy(d1,&r);}
EXPORT_SYMBOL_GPL(a52_p437_mark);
void a52_p437_arm_target(void){if(!current->group_leader||strcmp(current->group_leader->comm,"surfaceflinger"))return;atomic_cmpxchg(&a52_p437_target_pid,-1,current->pid);atomic_cmpxchg(&a52_p437_target_tgid,-1,current->tgid);}
EXPORT_SYMBOL_GPL(a52_p437_arm_target);
bool a52_p437_target_current(void){return atomic_read(&a52_p437_target_pid)==current->pid&&atomic_read(&a52_p437_target_tgid)==current->tgid;}
EXPORT_SYMBOL_GPL(a52_p437_target_current);
u64 a52_p437_boot_id(void){return READ_ONCE(a52_p437_id);} EXPORT_SYMBOL_GPL(a52_p437_boot_id);
static int __init a52_p437_init(void){u64 c,f;unsigned int i;BUILD_BUG_ON(sizeof(struct a52_p437_record)!=A52_P437_SLOT_BYTES);BUILD_BUG_ON(A52_P437_SLOTS*A52_P437_SLOT_BYTES!=A52_P437_COPY_BYTES);a52_p437_base=memremap(A52_P437_PHYS,A52_P437_BYTES,MEMREMAP_WB);if(!a52_p437_base)return 0;memset(a52_p437_base,0,A52_P437_BYTES);for(i=0;i<A52_P437_SLOTS;i++)atomic_set(&a52_p437_written[i],0);wmb();__flush_dcache_area(a52_p437_base,A52_P437_BYTES);dsb(sy);c=p437_cnt();f=p437_frq();a52_p437_id=c^(ktime_get_boottime_ns()<<1)^(u64)(unsigned long)a52_p437_init;if(!a52_p437_id)a52_p437_id=1;a52_p437_mark(P437_META,f,A52_P437_PHYS,NULL);return 0;}
core_initcall(a52_p437_init);

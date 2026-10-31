#ifndef G9_PROBE_H
#define G9_PROBE_H

#include <stdbool.h>
#include <stdint.h>

/* Temporary R7 baseline observer. Default dormant. All record storage is BSS;
 * only pump formats/sends. CPU occupancy is sampled, NOT switch accounting.
 * Wire rows: seq,lost,kind,span,tb_hi,tb_lo, then 18 kind-specific words.
 * 1 begin: owner,sampler,world,stage,mesh[7],completed_total,retired_total
 * 2 sample: id,role,effective,base,state,suspend,wait_queue,wait_token,
 *           main_phase,pc,late_us,missed
 * 3 end: pops,elapsed_us,samples,late,missed,mesh[7],completed_total,retired_total
 * 4 RPC: wait_id,transition(1 exhaustion/2 return/3 reacquire/4 cancel),
 *        thread_id,token_id,waiter_state,suspend,main_phase,effective,base
 * 5 thread: id,role (1 main,2 server,3 mesh,4 sender,5 watchdog)
 * 6 main pacing sleep: phase (thread_pacing_phase), observed foreign drain
 *        owner,requested_ms,wake_us,remaining_frame_ms. Timestamp is BEFORE
 *        sleep; span is active at wake publication (may end during sleep).
 * Mesh: queued,building,completed,oldest_us,submissions,stales,gathering.
 * Stage: 0 normal drain,1 checkpoint trial,2 snapshot,3 save,4 restore.
 * KThread state is raw SDK state; suspend!=0 is never runnable.
 * Logical LWP priorities (127 - kernel priority) on Wii; unknown=UINT32_MAX.
 */
#define G9_PROBE_WORDS 24u
#define G9_PROBE_CAPACITY 4096u
void g9_probe_configure(bool enabled, bool sampler);
bool g9_probe_enabled(void);
void g9_probe_role(uint32_t role);
void g9_probe_begin(uint32_t world, uint32_t stage);
void g9_probe_end(uint32_t pops, uint32_t elapsed_us);
void g9_probe_mesh(unsigned slot, unsigned state, bool stale);
void g9_probe_rpc(unsigned transition, const void* token);
struct thread_channel;
void g9_probe_rpc_pool(struct thread_channel* pool);
bool g9_probe_pump(bool (*emit)(const char*));
/* Portable sampler core: PC tests inject IRQ observations, never claim PC
 * scheduler fidelity. tb units are 1 us on PC, 1/60750000 s on Wii. */
void g9_probe_sample(uint64_t tb, const uint32_t observation[10]);
/* R14 observer only. Kind 7: frame,region,edge(1 begin/2 end),work,
 * backlog,oldest_us,phase,effective,base,mesh[7]. Unknown age = UINT32_MAX.
 * Regions 0 frame,1 phase,2 RPC drain,3 relight,4 flood resume,5 gather,
 * 6 submit,7 flip,8 finish,9 screen,10 render. Never interprets elapsed time as CPU time. */
void g9_probe_main_configure(bool enabled);
bool g9_probe_main_enabled(void);
void g9_probe_main_frame(bool begin);
void g9_probe_main_mark(unsigned region, bool begin, uint32_t work,
	uint32_t backlog, uint32_t oldest_us);
void g9_probe_main_phase(uint32_t phase);
void g9_probe_light_push(const void* world, unsigned depth);
void g9_probe_light_pop(const void* world, unsigned depth);
uint32_t g9_probe_light_age(const void* world, unsigned depth, bool flood);
void g9_probe_light_clear(const void* world);
void g9_probe_inbox(unsigned slot, bool queued);
void g9_probe_inbox_stats(uint32_t* count, uint32_t* age);
#endif

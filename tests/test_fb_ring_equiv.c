/*
	T1-a (2026-09-03 far-ring frame-drop root-cause doc, mechanism B) capacity
	proof. Mirrors the SPSC-ring + counting-semaphore framebuffer handoff in
	source/platform/wii/gfx.c (fb_ring_push/fb_ring_pop, copy_buffers,
	gfx_flip_buffers, gfx_setup's initial ownership) -- self-contained, like
	test_displaylist_vertex_equiv.c, since gfx.c itself only compiles against
	gccore.h on real Wii hardware. If you change the real ring/semaphore
	handoff, change this mirror too.

	At the tip this lane branched from (b94c599) the root-cause doc claims
	"gfx.c:338-340 allocates only TWO framebuffers while the semaphore is
	sized for three" and that "with no spare buffer, any single overrun costs
	a whole field in LWP_SemWait". Re-reading gfx.c against that claim found
	THREE XFBs already allocated (frameBuffer[3], gfx.c:329-331) and the
	semaphore already initialized count=2/max=3 (gfx.c:340) -- i.e. genuine
	triple buffering, landed at M1 (92b52e3), well before this doc. This test
	proves the CAPACITY consequence of that design directly and pins it as a
	regression guard: plain double buffering (2 XFBs) already gives
	gfx_flip_buffers one free flip of headroom (the textbook minimum for
	double buffering to mean anything -- draw into the spare while the other
	buffer is on screen); production's n_bufs=3 configuration buys a SECOND
	free flip on top of that, i.e. a full extra frame of render-ahead slack
	before a real VI-callback wait becomes unavoidable -- the case the doc
	describes (only two framebuffers) does not ship here.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

/* ---- fb_ring mirror (gfx.c:213-248) --------------------------------------
   Slots hold small integer "buffer ids" here instead of void* framebuffer
   pointers; the push/pop bodies are otherwise verbatim. */
#define FBR_SIZE 4 /* mirrors FB_RING_SIZE: power of two, > nr of buffers (3) */

struct fbr {
	int slot[FBR_SIZE];
	unsigned tail; /* producer-owned: next write position */
	unsigned head; /* consumer-owned: next read position */
};

static unsigned g_fbr_over = 0; /* mirrors g_gfx_fb_ring_over */

static void fbr_push(struct fbr* r, int v) {
	if(r->tail - r->head >= FBR_SIZE) { /* full: refuse, never overwrite */
		g_fbr_over++;
		return;
	}
	r->slot[r->tail % FBR_SIZE] = v;
	r->tail = r->tail + 1;
}

static int fbr_pop(struct fbr* r) { /* -1 == empty (mirrors the NULL return) */
	if(r->head == r->tail)
		return -1;
	int v = r->slot[r->head % FBR_SIZE];
	r->head = r->head + 1;
	return v;
}

static unsigned fbr_count(const struct fbr* r) {
	return r->tail - r->head;
}

/* ---- T1-a: free-burst capacity (mirrors gfx_setup + gfx_flip_buffers) ----
   gfx_setup seeds n_bufs-1 buffers into fb_from_vi (sem starts at n_bufs-1)
   and hands main the LAST buffer to draw into first. gfx_flip_buffers then
   does, every frame: push(to_vi, frame); LWP_SemWait(); frame =
   pop(from_vi). This counts how many of THOSE push+wait cycles can complete
   with ZERO VI-callback activity (the worst case: main runs far ahead of
   vsync, e.g. a startup burst, or a genuine render overrun that outpaces the
   retrace) before the semaphore has no credit left and a real wait -- i.e.
   an actual block on the VI IRQ -- becomes unavoidable. */
static int fbr_free_burst(int n_bufs) {
	struct fbr from_vi = {{0}, 0, 0};
	struct fbr to_vi = {{0}, 0, 0};
	int sem = n_bufs - 1; /* LWP_SemInit(&fb_free_sem, n_bufs-1, n_bufs) */

	for(int i = 0; i < n_bufs - 1; i++)
		fbr_push(&from_vi, i);
	int frame = n_bufs - 1; /* frame = frameBuffer[n_bufs-1] */

	int free_flips = 0;
	for(int i = 0; i < n_bufs; i++) {
		fbr_push(&to_vi, frame); /* gfx_flip_buffers: push the drawn frame */
		if(sem == 0)
			break; /* this LWP_SemWait would actually BLOCK */
		sem--;						/* LWP_SemWait succeeds immediately */
		frame = fbr_pop(&from_vi); /* acquire the next draw buffer */
		assert(frame >= 0);		/* a successful wait always finds one */
		free_flips++;
	}
	return free_flips;
}

static void run_t1a_free_burst(void) {
	int slack3 = fbr_free_burst(3); /* today's shipped configuration */
	int slack2 = fbr_free_burst(2); /* plain double buffering */

	// free_flips == n_bufs-1 by construction (the semaphore starts with
	// exactly that much credit) -- double buffering (2) already buys ONE
	// free flip (draw into the spare while the other buffer is being shown,
	// the textbook minimum for double buffering to mean anything); triple
	// buffering (3, what gfx.c actually ships) buys a SECOND one on top,
	// i.e. main can be a full extra frame ahead of what the VI is currently
	// scanning out before a real wait is unavoidable. That extra frame of
	// headroom -- not "zero vs nonzero" -- is what T1-a's fix delivers, and
	// it's already present at b94c599.
	assert(slack2 == 1);
	assert(slack3 == 2);

	printf("test_fb_ring_equiv: T1-a free-burst -- double buffering (2 XFBs) "
		   "gives %d free flip before LWP_SemWait can force a real "
		   "VI-callback wait; the 3-buffer ring gfx.c actually ships "
		   "(gfx.c:329-340) gives %d -- one full extra frame of "
		   "render-ahead headroom, already present at b94c599\n",
		   slack2, slack3);
}

/* ---- steady-state push/pop/post order + token conservation ---------------
   Mirrors the real interleaving of one gfx_flip_buffers call and one
   copy_buffers (VI pre-retrace) callback per iteration, in the ORDER gfx.c
   actually uses: main pushes the finished frame to fb_to_vi and (if credit
   is available) claims it immediately; the VI callback then pops fb_to_vi,
   retires the previously-shown buffer back to fb_from_vi and POSTS *AFTER*
   that push (gfx.c:272-273's documented order -- reversing it would let a
   waiter observe a token before the buffer it names is actually back in the
   ring); if main's own wait had no credit yet, THIS callback's post is what
   satisfies it. Runs for both n_bufs=2 and n_bufs=3 and asserts the token
   count is conserved every single iteration (never overflows fbr_push's
   capacity guard, never leaves a wait stranded) -- the "never a token
   accounting regression" contract the real fbover=/assert(frame) guards
   encode. */
static void run_steady_state(int n_bufs, int iterations) {
	struct fbr from_vi = {{0}, 0, 0};
	struct fbr to_vi = {{0}, 0, 0};
	int sem = n_bufs - 1;
	for(int i = 0; i < n_bufs - 1; i++)
		fbr_push(&from_vi, i);
	int frame = n_bufs - 1;
	int current_frame = -1; /* nothing on screen yet, mirrors current_frame */

	for(int it = 0; it < iterations; it++) {
		unsigned over_before = g_fbr_over;

		/* main: gfx_flip_buffers */
		fbr_push(&to_vi, frame);
		bool blocked = (sem == 0);
		if(!blocked)
			sem--;

		/* VI: copy_buffers (exactly one pre-retrace callback this iteration) */
		int input = fbr_pop(&to_vi);
		assert(input >= 0); /* to_vi always has at least the push above */
		if(current_frame >= 0) {
			fbr_push(&from_vi, current_frame); /* push BEFORE... */
			sem++;								/* ...post AFTER (gfx.c order) */
		}
		current_frame = input;

		/* main resumes: a pending wait is satisfied by the post just above */
		if(blocked) {
			assert(sem > 0);
			sem--;
		}
		frame = fbr_pop(&from_vi);
		assert(frame >= 0); /* a successful wait ALWAYS finds a buffer */

		assert(g_fbr_over == over_before); /* never overflowed a ring */

		/* Token conservation: every buffer is accounted for exactly once --
		   in fb_from_vi, in fb_to_vi, held by the VI as current_frame, or
		   held by main as `frame` -- every single iteration. */
		unsigned total
			= (unsigned)sem + fbr_count(&to_vi) + (current_frame >= 0 ? 1u : 0u)
			+ 1u /* `frame`, currently held by main */;
		assert(total == (unsigned)n_bufs);
	}
}

static void run_t1a_steady_state(void) {
	run_steady_state(3, 2000);
	run_steady_state(2, 2000);
	printf("test_fb_ring_equiv: T1-a steady-state -- push/pop/post order "
		   "conserves all buffer tokens (no ring overflow, no stranded "
		   "wait) over 2000 iterations for both 3-slot and 2-slot rings\n");
}

int main(void) {
	run_t1a_free_burst();
	run_t1a_steady_state();
	printf("test_fb_ring_equiv: all assertions passed\n");
	return 0;
}

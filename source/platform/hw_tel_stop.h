/* Shared stop ordering for the Wii worker and native socket doubles. */
#ifndef CW_HW_TEL_STOP_H
#define CW_HW_TEL_STOP_H
#include <stdbool.h>
#include <stdint.h>

#define CW_TEL_STOP_MS 250u
/* Leave 50 ms of the existing stop budget for cancellation/join. */
#define CW_TEL_DRAIN_MS 200u
struct cw_tel_stop {
	bool requested, cancelled, joined;
};

/* Only the stop worker calls this. Neither IOS cancellation nor join has a
 * timeout in libogc, so neither is allowed on the main thread. */
static inline void cw_tel_stop_run(struct cw_tel_stop* st,
	void (*cancel)(void*), void (*join)(void*), void* user) {
	cancel(user);
	__atomic_store_n(&st->cancelled, true, __ATOMIC_RELEASE);
	join(user);
	__atomic_store_n(&st->joined, true, __ATOMIC_RELEASE);
}

/* A timeout retains all worker/socket storage until reset. It never cancels
 * a thread or destroys resources that a late IOS completion can still use. */
static inline bool cw_tel_stop_wait(struct cw_tel_stop* st,
	uint64_t (*now_ms)(void*), void (*sleep_ms)(void*), void* user) {
	uint64_t start = now_ms(user);
	while(!__atomic_load_n(&st->joined, __ATOMIC_ACQUIRE)) {
		if(now_ms(user) - start >= CW_TEL_STOP_MS) return false;
		sleep_ms(user);
	}
	return true;
}

/* Admission is already closed. Give the sender a bounded opportunity to
 * finish every admitted record before the stop worker interrupts IOS. Main
 * still spends at most one 250 ms budget across drain and cancellation. */
static inline bool cw_tel_stop_orderly(struct cw_tel_stop* st,
	bool (*drained)(void*), uint64_t (*now_ms)(void*),
	void (*sleep_ms)(void*), void* user) {
	uint64_t start = now_ms(user);
	while(!__atomic_load_n(&st->requested, __ATOMIC_ACQUIRE)
			&& !drained(user) && now_ms(user) - start < CW_TEL_DRAIN_MS)
		sleep_ms(user);
	__atomic_store_n(&st->requested, true, __ATOMIC_SEQ_CST);
	while(!__atomic_load_n(&st->joined, __ATOMIC_ACQUIRE)) {
		if(now_ms(user) - start >= CW_TEL_STOP_MS) return false;
		sleep_ms(user);
	}
	return true;
}
#endif

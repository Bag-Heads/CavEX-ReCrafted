/* PC pthread stand-ins for the five thread.h calls beta_import.c makes, so
 * the importer tests link without thread.c's telemetry/G9 dependencies.
 * Same semantics as thread.c's PLATFORM_PC arm (priority is a no-op there). */

#include <pthread.h>
#include <string.h>

#include "platform/thread.h"

bool thread_create(struct thread* t, void* (*entry)(void* arg), void* arg,
				   uint8_t priority) {
	(void)priority;
	memset(t, 0, sizeof(*t));
	t->created = pthread_create(&t->native, NULL, entry, arg) == 0;
	return t->created;
}

void thread_join(struct thread* t) {
	if(t->created)
		pthread_join(t->native, NULL);
	t->created = false;
}

bool thread_mutex_init(struct thread_mutex* m) {
	m->initialized = pthread_mutex_init(&m->native, NULL) == 0;
	return m->initialized;
}

void thread_mutex_lock(struct thread_mutex* m) {
	pthread_mutex_lock(&m->native);
}

void thread_mutex_unlock(struct thread_mutex* m) {
	pthread_mutex_unlock(&m->native);
}

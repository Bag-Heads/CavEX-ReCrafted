/*
	Inert stand-ins for the opt-in deferred cave-ambience seam (C2 P2), for the
	tests that textually #include source/platform/audio.c with their OWN stubbed
	sound registry (test_menu_sound_events, test_menu_sound_priority). They
	model exactly what production does with "audio.deferred_cave" off -- the
	module is never brought up: nothing is ever ready, no slot is ever the
	deferred voice, every notification is a no-op -- so those tests keep
	exercising the menu-sound path they exist for, unchanged.

	Include AFTER "../source/platform/audio.c". The real module is covered by
	test_audio_deferred and test_audio_deferred_registry.
*/
#ifndef TESTS_SUPPORT_AUDIO_DEFERRED_STUBS_H
#define TESTS_SUPPORT_AUDIO_DEFERRED_STUBS_H

#include "platform/audio.h"
#include "platform/audio_deferred.h"

/* R19 added this device query. These five focused fake devices never play
 * music and have no queued samples: their conservative reserve is zero. */
static uint32_t audio_backend_music_reserve_us(void) { return 0; }
/* G9 S1 service seams: the deferred module is never up (idle), and these
 * fake devices give no music witness (no AUDIO completion credit). */
bool audio_deferred_service_idle(void) { return true; }
bool audio_music_service_witness(struct audio_service_witness* w) {
	*w = (struct audio_service_witness){0};
	return false;
}

bool audio_deferred_request(const struct audio_deferred_source* src,
							uint16_t sound_id, int variant, const float pos[3],
							float vol, float pitch) {
	(void)src, (void)sound_id, (void)variant, (void)pos, (void)vol, (void)pitch;
	return false;
}
bool audio_deferred_take_ready(struct audio_deferred_ready* out) {
	(void)out;
	return false;
}
void audio_deferred_note_started(uint32_t req_id, int slot) {
	(void)req_id, (void)slot;
}
void audio_deferred_note_not_started(uint32_t req_id,
									 enum audio_deferred_outcome why) {
	(void)req_id, (void)why;
}
void audio_deferred_note_voice_released(int slot,
										enum audio_deferred_outcome why) {
	(void)slot, (void)why;
}
int audio_deferred_voice_slot(void) { return -1; }
void audio_deferred_cancel(void) {}
void audio_deferred_shutdown(void) {}

const struct audio_deferred_source*
audio_registry_deferred_source(uint16_t sound_id,
							   const struct audio_sample* smp, int* variant) {
	(void)sound_id, (void)smp, (void)variant;
	return NULL;
}
void audio_registry_note_deferred_started(int slot, uint16_t sound_id,
										  int variant,
										  const struct audio_sample* smp) {
	(void)slot, (void)sound_id, (void)variant, (void)smp;
}
void audio_registry_deferred_telemetry(void) {}
const struct audio_sample* audio_registry_variant(uint16_t sound_id,
												  int variant) {
	(void)sound_id, (void)variant;
	return NULL;
}
int audio_registry_variant_count(uint16_t sound_id) {
	(void)sound_id;
	return 0;
}

#endif

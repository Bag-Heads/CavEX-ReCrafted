/* Approved one-frame priority arbitration at the real dispatcher boundary,
 * plus the per-device controller-notice rules. Every assertion below is
 * FRAME-BATCHED where the spec's "For competing same-frame events, emit at
 * most one cue" is what is under test: several events are submitted inside a
 * single 16.7 ms frame and only then flushed. */
#include <assert.h>
#include <stdio.h>
#include "platform/audio.h"
#include "platform/menu_sound.h"
#undef PLATFORM_PC
static unsigned starts, picked, last_id;
static int last_slot = -1;
static bool playing = true;
static uint32_t audio_backend_music_last_dur_ms(void) { return 0; }
#include "../source/platform/audio.c"
#include "support/audio_deferred_stubs.h" /* C2 P2 seam, inert: opt-in off */
static bool audio_backend_init(void) { return true; }
static void audio_backend_shutdown(void) {}
static void audio_backend_voice_start(int slot, const struct audio_sample* smp,
    float rate, float l, float r) {
    assert(smp && rate == 48000 && l == r);
    if(last_slot >= 0) assert(slot == last_slot); /* one dedicated menu voice */
    last_slot = slot; starts++; last_id = picked;
}
static void audio_backend_voice_gain(int slot, float l, float r) {}
static bool audio_backend_voice_playing(int slot) { return playing; }
static void audio_backend_stop_all(void) {}
static void audio_backend_music_start(const int16_t* pcm, uint32_t frames, float rate, float gain) {}
static void audio_backend_music_gain(float gain) {}
static bool audio_backend_music_playing(void) { return false; }
static bool audio_backend_music_drained(void) { return true; }
static void audio_backend_music_stop(void) {}
static uint32_t audio_backend_music_native_rate(void) { return 0; }
/* Diagnostics-only registry hook audio.c calls after every voice start
   (audio.h): the real one lives in audio_registry.c, which these
   include-the-TU tests deliberately do not link -- same reason
   audio_registry_load/_pick are stubbed just below. */
void audio_registry_note_voice_started(int slot,
                                       const struct audio_sample* smp) {
    (void)slot;
    (void)smp;
}
void audio_registry_load(void) {}
const struct audio_sample* audio_registry_pick(uint16_t id) {
    static const int16_t pcm[] = {0, 1, 0};
    static const struct audio_sample sample = {.pcm = pcm, .frames = 3, .rate = 48000};
    picked = id; return &sample;
}
/* Three Wiimotes and a wired GameCube pad, named the way the input layer
 * names them (struct input_device {kind, channel}, input_owner.h). */
static struct input_device wpad(unsigned channel) {
    return (struct input_device){.kind = INPUT_DEVICE_WPAD, .channel = channel};
}
static struct input_device gcpad(unsigned channel) {
    return (struct input_device){.kind = INPUT_DEVICE_GAMECUBE, .channel = channel};
}
static ptime_t at(unsigned ms) { return time_add_ms((ptime_t){0}, ms); }
static void emit(enum menu_sound_event e, unsigned ms) { audio_menu_event(e, at(ms)); }
static void demit(enum menu_sound_event e, struct input_device d, unsigned ms) {
    audio_menu_device_event(e, d, at(ms));
}
static void flush(void) { audio_update((vec3){0}, 0); }

int main(void) {
    const struct input_device p1 = wpad(0), p2 = wpad(1), p3 = wpad(2);
    audio_init(true, 100);

    /* ONE frame, five competing events: "newly blocking controller attention,
     * committed action, other controller notice, menu transition, then
     * navigation" -- and "emit at most one cue". */
    emit(MENU_SOUND_FOCUS, 0);
    emit(MENU_SOUND_SECTION, 0);
    demit(MENU_SOUND_CONNECTED, p1, 4);
    emit(MENU_SOUND_ITEM, 8);
    demit(MENU_SOUND_ATTENTION, p1, 12);
    flush();
    assert(starts == 1 && last_id == SOUND_MENU_ATTENTION);

    demit(MENU_SOUND_ATTENTION, p2, 200);
    emit(MENU_SOUND_SELECT, 204);
    demit(MENU_SOUND_CONNECTED, p2, 208);
    flush();
    assert(starts == 2 && last_id == SOUND_MENU_ATTENTION);

    emit(MENU_SOUND_FOCUS, 400);
    demit(MENU_SOUND_CONNECTED, p3, 404);
    emit(MENU_SOUND_TYPE, 408);
    emit(MENU_SOUND_SECTION, 412);
    flush();
    assert(starts == 3 && last_id == SOUND_MENU_TYPE);

    emit(MENU_SOUND_SELECT, 600); flush();
    assert(starts == 4 && last_id == SOUND_MENU_SELECT);
    demit(MENU_SOUND_BATTERY_ATTENTION, p1, 700); flush();
    assert(starts == 4); /* committed action remains audible; notice is discarded */

    playing = false;
    demit(MENU_SOUND_BATTERY_ATTENTION, p1, 800); flush();
    assert(starts == 5 && last_id == SOUND_MENU_ATTENTION);
    assert(last_slot >= 0 && audio_voices_active() == 0); /* reaped after start */

    /* "Once per low-battery episode ... No periodic chirp": a caller that
     * POLLS the battery instead of edge-triggering it still gets one cue. */
    for(unsigned ms = 900; ms < 2000; ms += 20)
        { demit(MENU_SOUND_BATTERY_ATTENTION, p1, ms); flush(); }
    assert(starts == 5);
    /* "rearmed only after a stable recovery/replacement" -- device layer. */
    audio_menu_battery_rearm(p1);
    demit(MENU_SOUND_BATTERY_ATTENTION, p1, 2000); flush();
    assert(starts == 6 && last_id == SOUND_MENU_ATTENTION);

    /* PER-DEVICE battery latch. A process-wide latch made P1's chirp swallow
     * P2's low battery, and made P1's recovery re-arm P2 for a second chirp
     * inside one episode. Both are now impossible. */
    demit(MENU_SOUND_BATTERY_ATTENTION, p2, 2100); flush();
    assert(starts == 7); /* P2 is a different device: NOT swallowed by P1 */
    demit(MENU_SOUND_BATTERY_ATTENTION, p1, 2120); flush();
    assert(starts == 7); /* P1 still latched from 2000 */
    demit(MENU_SOUND_BATTERY_ATTENTION, p2, 2140); flush();
    assert(starts == 7); /* P2 latched by its own cue */
    audio_menu_battery_rearm(p1); /* only P1 recovers */
    demit(MENU_SOUND_BATTERY_ATTENTION, p2, 2160); flush();
    assert(starts == 7); /* P1's recovery does NOT re-arm P2 */
    demit(MENU_SOUND_BATTERY_ATTENTION, p1, 2180); flush();
    assert(starts == 8); /* P1's own episode may start again */
    audio_menu_battery_rearm(p1); /* idempotent when already armed */
    audio_menu_battery_rearm(p2);
    demit(MENU_SOUND_BATTERY_ATTENTION, p2, 2200); flush();
    assert(starts == 9);

    /* GUARDS RUN BEFORE ARBITRATION. A Back inside its 90 ms navigation
     * window must not win the frame and then die to that window, silencing a
     * genuine device cue the spec requires "once for a genuine detected/
     * usable device event". The frame falls through to the highest-priority
     * candidate that can actually play. */
    playing = true;
    emit(MENU_SOUND_BACK, 3000); flush();
    assert(starts == 10 && last_id == SOUND_MENU_BACK); /* arms nav -> 3090 */
    demit(MENU_SOUND_CONNECTED, p1, 3010);  /* same 16.7 ms frame ... */
    emit(MENU_SOUND_BACK, 3017);            /* ... higher priority, rate-limited */
    flush();
    assert(starts == 11 && last_id == SOUND_MENU_CONNECTED);
    emit(MENU_SOUND_BACK, 3030);            /* submit order reversed: same result */
    demit(MENU_SOUND_CONNECTED, p2, 3034);
    flush();
    assert(starts == 12 && last_id == SOUND_MENU_CONNECTED);

    /* Navigation + Back in ONE frame, both passing their guards: Back is the
     * committed action, so it "replaces incidental focus sounds caused by
     * that action" -- exactly one cue, and it is Back. */
    emit(MENU_SOUND_FOCUS, 3200);
    emit(MENU_SOUND_BACK, 3208);
    flush();
    assert(starts == 13 && last_id == SOUND_MENU_BACK); /* arms nav -> 3298 */
    emit(MENU_SOUND_FOCUS, 3217); flush();
    assert(starts == 13); /* the 90 ms navigation limit still bites alone */

    /* TWO DEVICES' BATTERY IN ONE FRAME. "Emit at most one cue" wins, and
     * "skipped sounds are discarded, never queued" -- so the loser's latch is
     * NOT consumed and its own episode can still be heard on a later frame. */
    playing = false; flush(); /* reap the menu voice; no committed cue playing */
    audio_menu_battery_rearm(p1);
    audio_menu_battery_rearm(p2);
    demit(MENU_SOUND_BATTERY_ATTENTION, p1, 3400);
    demit(MENU_SOUND_BATTERY_ATTENTION, p2, 3408);
    flush();
    assert(starts == 14 && last_id == SOUND_MENU_ATTENTION);
    demit(MENU_SOUND_BATTERY_ATTENTION, p2, 3417); flush();
    assert(starts == 15); /* P2 was discarded, not latched */
    demit(MENU_SOUND_BATTERY_ATTENTION, p1, 3434); flush();
    assert(starts == 15); /* P1 played, so P1 is latched */

    /* ENFORCED controller-notice collapsing (the caller contract is no longer
     * only prose): "Collapse discovery and player assignment in the same
     * connection flow into one cue" / "Do not repeat while waiting or
     * retrying unchanged state" -- same device, same cue, inside
     * MENU_SOUND_NOTICE_COLLAPSE_MS, dropped and counted. */
    assert(audio_menu_notices_collapsed() == 0);
    demit(MENU_SOUND_CONNECTED, p1, 4000); flush();
    assert(starts == 16 && last_id == SOUND_MENU_CONNECTED);
    demit(MENU_SOUND_CONNECTED, p1, 4017); flush(); /* assignment half */
    assert(starts == 16 && audio_menu_notices_collapsed() == 1);
    demit(MENU_SOUND_CONNECTED, p2, 4034); flush(); /* a SECOND device */
    assert(starts == 17 && audio_menu_notices_collapsed() == 1);
    demit(MENU_SOUND_ATTENTION, p1, 4051); flush(); /* a real state change */
    assert(starts == 18 && last_id == SOUND_MENU_ATTENTION);
    demit(MENU_SOUND_ATTENTION, p1, 4068); flush(); /* retrying unchanged state */
    assert(starts == 18 && audio_menu_notices_collapsed() == 2);
    demit(MENU_SOUND_CONNECTED, p1, 4000 + MENU_SOUND_NOTICE_COLLAPSE_MS);
    flush();
    assert(starts == 19 && audio_menu_notices_collapsed() == 2);

    /* The other two enforced halves of the contract, both release-safe. */
    assert(audio_menu_contract_violations() == 0);
    /* "never for a wired GameCube controller" -- dropped, not chirped. */
    demit(MENU_SOUND_BATTERY_ATTENTION, gcpad(0), 4700); flush();
    assert(starts == 19 && audio_menu_contract_violations() == 1);
    /* An out-of-range device is counted but never silently loses its cue. */
    demit(MENU_SOUND_CONNECTED, wpad(7), 4720); flush();
    assert(starts == 20 && audio_menu_contract_violations() == 2);
    audio_menu_battery_rearm(wpad(7));
    assert(audio_menu_contract_violations() == 3);

    puts("PASS menu priority: attention > committed > notice > transition > navigation; one voice; guards before arbitration (rate-limited Back never silences a device cue); per-device battery latch + same-frame discard; enforced per-device notice collapse and contract counters");
    return 0;
}

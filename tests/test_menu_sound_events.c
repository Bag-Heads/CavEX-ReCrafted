/* Actual shared dispatcher, deterministic device boundary. No speaker required. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sha256.h"
#include "platform/audio.h"
#include "platform/menu_sound.h"
/* Keep PC time's ABI, substitute only the platform device in this focused TU. */
#undef PLATFORM_PC
static unsigned starts, picked, last_id;
static float last_gain;
static bool missing, device_ok = true;
static uint32_t audio_backend_music_last_dur_ms(void) { return 0; }
#include "../source/platform/audio.c"
#include "support/audio_deferred_stubs.h" /* C2 P2 seam, inert: opt-in off */
static bool audio_backend_init(void) { return device_ok; }
static void audio_backend_shutdown(void) {}
static void audio_backend_voice_start(int slot, const struct audio_sample* smp,
    float rate, float l, float r) {
    assert(slot >= 0 && slot < 16 && smp && rate == 48000 && l == r);
    starts++; last_gain = l; last_id = picked;
}
static void audio_backend_voice_gain(int slot, float l, float r) { assert(l == r); last_gain = l; }
static bool audio_backend_voice_playing(int slot) { return true; }
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
    picked = id; return missing ? NULL : &sample;
}
static void event(enum menu_sound_event e, unsigned ms) {
    audio_menu_event(e, time_add_ms((ptime_t){0}, ms));
}
/* The controller three now name their device (menu_sound.h caller contract). */
static struct input_device wpad(unsigned channel) {
    return (struct input_device){.kind = INPUT_DEVICE_WPAD, .channel = channel};
}
static void device_event(enum menu_sound_event e, unsigned channel, unsigned ms) {
    audio_menu_device_event(e, wpad(channel), time_add_ms((ptime_t){0}, ms));
}
static void flush(void) { audio_update((vec3){0}, 0); }

static const char* const registry_names[AUDIO_SOUND_COUNT] = {
#define AUDIO_SOUND_TABLE_X(id, name) [id] = name,
    AUDIO_SOUND_TABLE(AUDIO_SOUND_TABLE_X)
#undef AUDIO_SOUND_TABLE_X
};

static void check_asset(uint16_t id, const char* file, const char* expected) {
    char subpath[128], path[160], actual[CW_SHA256_HEX_LENGTH + 1];
    assert(id < AUDIO_SOUND_COUNT && registry_names[id]);
    assert(audio_sound_table_subpath(registry_names[id], subpath,
                                    sizeof(subpath)) > 0);
    snprintf(path, sizeof(path), "assets/%s.wav", subpath);
    assert(strcmp(path + strlen(path) - strlen(file), file) == 0);
    FILE* f = fopen(path, "rb");
    assert(f);
    cw_sha256_ctx ctx;
    cw_sha256_init(&ctx);
    unsigned char bytes[4096];
    size_t n;
    while((n = fread(bytes, 1, sizeof(bytes), f)) != 0)
        cw_sha256_update(&ctx, bytes, n);
    assert(!ferror(f));
    fclose(f);
    cw_sha256_final_hex(&ctx, actual);
    assert(strcmp(actual, expected) == 0);
}

static void check_assets(void) {
    check_asset(SOUND_MENU_SCROLL, "scroll.wav", "6c4f522b9f288077c2a877b7cbde5d4e2cb3ef934d51b6b516161235e0cf5b94");
    check_asset(SOUND_MENU_SECTION, "category.wav", "4ab8fc44112928de9be88bbbc63b7f106a6fbb50b3eee998259f12384c1d5fd0");
    check_asset(SOUND_MENU_SELECT, "select.wav", "71d562a40fe4dc5867020fc538e3c947265ffd7ea6bc3c94f9bb04290d54fe6e");
    check_asset(SOUND_MENU_BACK, "back.wav", "f77469225c045851cf6b8561291d339265b3556fa48329a2d705a07fe2ba7ad3");
    check_asset(SOUND_MENU_ITEM, "item.wav", "c9773d76ae9db8c2fcdfe48a7552808fb97218e287728cd2eb3ade5111ae41f8");
    check_asset(SOUND_MENU_TYPE, "type.wav", "3ba7678d1237902c765e4efaa6960a9f52d72aa74e40625eee9a6eeab1b5b800");
    check_asset(SOUND_MENU_CONNECTED, "connected.wav", "eb95def2f8e29ae6a2a36fb9b73495dc0e86d459a80a810237cbef96f37d0fe4");
    check_asset(SOUND_MENU_ATTENTION, "attention.wav", "f75f3531daf9489a7ca6c7a7cf4f44ec876250216f2357a620d1904933150880");
}
int main(void) {
    check_assets();
    audio_init(true, 40);
    event(MENU_SOUND_FOCUS, 0); assert(starts == 0); flush();
    assert(starts == 1 && last_id == SOUND_MENU_SCROLL);
    assert(last_gain == audio_ui_loudness(1, .4F));
    event(MENU_SOUND_FOCUS, 89); flush(); assert(starts == 1);
    event(MENU_SOUND_FOCUS, 90); flush(); assert(starts == 2);
    event(MENU_SOUND_SECTION, 179); flush(); assert(starts == 2); /* other player, shared guard */
    event(MENU_SOUND_SELECT, 91); flush(); assert(starts == 3 && last_id == SOUND_MENU_SELECT);
    event(MENU_SOUND_SELECT, 150); flush(); assert(starts == 3);
    event(MENU_SOUND_SELECT, 151); flush(); assert(starts == 4);
    event(MENU_SOUND_BACK, 270); flush(); assert(starts == 4);
    event(MENU_SOUND_BACK, 271); flush(); assert(starts == 5 && last_id == SOUND_MENU_BACK);

    event(MENU_SOUND_ITEM, 400); flush(); assert(starts == 6 && last_id == SOUND_MENU_ITEM);
    event(MENU_SOUND_TYPE, 489); flush(); assert(starts == 6);
    event(MENU_SOUND_TYPE, 490); flush(); assert(starts == 7 && last_id == SOUND_MENU_TYPE);
    device_event(MENU_SOUND_CONNECTED, 0, 500); flush(); assert(starts == 8 && last_id == SOUND_MENU_CONNECTED);
    device_event(MENU_SOUND_ATTENTION, 0, 510); flush(); assert(starts == 9 && last_id == SOUND_MENU_ATTENTION);
    device_event(MENU_SOUND_BATTERY_ATTENTION, 0, 520); flush();
    assert(starts == 10 && last_id == SOUND_MENU_ATTENTION);
    assert(last_gain == audio_ui_loudness(.65F, .4F));
    assert(audio_voices_active() == 1);

    event(MENU_SOUND_SELECT, 600); flush(); assert(starts == 11);
    assert(audio_set_master_volume(20));
    assert(last_gain == audio_ui_loudness(1, .2F));
    device_event(MENU_SOUND_CONNECTED, 1, 610); /* channel 1: not channel 0's collapse window */
    assert(audio_set_master_volume(0)); flush(); assert(starts == 11); /* pending cancelled */
    assert(audio_voices_active() == 0);
    assert(audio_set_master_volume(80)); event(MENU_SOUND_SELECT, 700); flush();
    assert(starts == 12 && last_gain == audio_ui_loudness(1, .8F));
    missing = true; event(MENU_SOUND_SELECT, 760); flush(); assert(starts == 12);
    missing = false; event(MENU_SOUND_SELECT, 760); flush(); assert(starts == 13); /* failed play consumes no guard */

    /* Split-screen chest sorts share this one dispatcher: two local players
     * whose sorts commit in the SAME frame emit at most one cue ("For
     * competing same-frame events, emit at most one cue"; "Use one menu-audio
     * mixer and one volume/mute setting shared by both split-screen
     * players"), and a third completion inside the 60 ms confirmation guard
     * is dropped, never queued. */
    event(MENU_SOUND_SELECT, 900); event(MENU_SOUND_SELECT, 900);
    flush(); assert(starts == 14 && last_id == SOUND_MENU_SELECT);
    event(MENU_SOUND_SELECT, 940); flush(); assert(starts == 14);
    event(MENU_SOUND_SELECT, 1020); flush(); assert(starts == 15);
    /* A completed transfer for the other player rides the shared 90 ms
     * item/type repeat guard the same way. */
    event(MENU_SOUND_ITEM, 1100); event(MENU_SOUND_ITEM, 1100);
    flush(); assert(starts == 16 && last_id == SOUND_MENU_ITEM);
    event(MENU_SOUND_ITEM, 1150); flush(); assert(starts == 16);
    event(MENU_SOUND_ITEM, 1200); flush(); assert(starts == 17);
    audio_shutdown(); audio_init(false, 100); event(MENU_SOUND_FOCUS, 0); flush(); assert(starts == 17);
    device_ok = false; audio_init(true, 100); event(MENU_SOUND_FOCUS, 0); flush(); assert(starts == 17);
    puts("PASS eight menu cues/assets: SHA-256, shared guards/voice, battery gain, mute/volume, silent failures");
    return 0;
}

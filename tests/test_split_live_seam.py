"""Run main.c's REAL split-screen seams -- the guest-maintenance block and the
config-flush falling edge -- against stubs, on the host.

R-A9 (mp-split-0912) left three test gaps whose subject is main.c itself:

  gap 4  tests/test_split_live_options.c case (k) pins the two guest predicates,
         not main.c's USE of them. `guest_joining`'s membership set, the
         one-settle latch, its A9-M4 reset placement and the
         refuse -> settle(false) -> request_local_leave SEQUENCE all live in the
         main loop, so they were structurally untestable.
  gap 5  nothing exercised the falling-edge latch: `split_world_was_loaded` is
         main-loop-local and case (l) calls screen_options_split_flush()
         directly, so the EDGE -- and A9-L9's mid-play `world_loaded` clear with
         screen_ingame still current -- was unproven.

The split-OFF transaction adds a third programmable predicate. This harness
also slices the shipping physical-arm expression and proves that predicate
closes both it and the later claimed/synthetic join-request gate.

Both are closed here the way test_anchor_measurement_clock.py closes the
profile-completion block: the harness SLICES THE PRODUCTION TEXT out of
source/main.c and compiles it. Nothing is copied or re-modelled, so this cannot
drift from main.c, and no seam had to be extracted out of a delicate block to
make it testable (a pure-move extraction would have tested the copy, not the
shipping code). `enum client_guest_phase` is sliced out of
source/game/client_session.h for the same reason -- a phase added there reaches
the sweep below instead of being silently unclassified.

The three predicates screen_options.c owns are STUBS with programmable returns
that RECORD their arguments: what is under test here is the arguments main.c
computes and the order in which it acts on the answers. The predicate bodies
themselves are pinned against the real file by case (k).
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* G9 service observer (default OFF) is compiled out of the slices. */
#define SERVICE_OBSERVER(call) ((void)0)

/* GUEST_PHASE_ENUM (sliced from source/game/client_session.h) */
/* GUEST_PHASE_ENUM */

/* A phase added to that enum must be classified below, not swept silently. */
_Static_assert(CLIENT_GUEST_FAILED == 6,
    "client_guest_phase changed: extend expect_joining() and the sweep");

/* ---- the shapes the sliced text dereferences ---------------------------- */
struct screen { void (*render3D)(struct screen*, const float*); void (*update)(struct screen*, float); };
static struct screen screen_ingame, screen_death, screen_inventory,
    screen_crafting, screen_chest, screen_furnace, screen_dispenser,
    screen_iron_chest, screen_sign, screen_load_world, screen_creative_picker;
static void screen_set(struct screen*);
/* LOADING_TYPES (sliced from source/network/client_frame_worlds.h) */
/* LOADING_TYPES */
struct world { int fixture_identity; };
/* Only the current context identity read by main.c's respawn seam is needed. */
#define CLIENT_CTX_SLOT_COUNT 2
struct client_world_context { uint32_t server_generation; int dimension; };
struct client_ctx_registry { int fixture_identity; };
struct local_pause { bool active; };
struct split_layout_rect { int y, height; };
struct input_player_frame { bool connected; };
enum { LOCAL_MENU_CREATIVE = 5 };
struct creative_picker { int fixture_selection; };
struct local_menu_host {
    int mode;
    struct creative_picker picker;
    bool active;
    bool split_owned;
    uint8_t window;
    void* window_identity;
};
struct local_player_data { bool capture_input; };
struct entity { unsigned id; int health; struct { struct local_player_data local_player; } data; };
struct camera { float x, y, z; int fixture_identity; float view[16]; };
struct death_ui { bool active; };
struct client_session {
    int local_phase;
    uint64_t local_ticket;
    uint64_t local_generation;
    bool local_bound;
    bool local_gameplay_ready;
    bool local_reply_failed;
    bool local_join_failed;
    bool local_respawn_loading;
    bool local_respawn_ack_send_pending;
    uint64_t local_respawn_epoch;
    uint64_t local_respawn_completed_epoch;
    uint8_t local_respawn_world_slot;
    uint32_t local_respawn_world_generation;
    int8_t local_respawn_dimension;
    struct screen* current_screen;
    struct entity* local_player;
    void* windows[256];
    struct local_menu_host menu_host;
    struct local_pause pause;
    struct death_ui death;
    int visible_window;
    struct camera camera;
    bool local_respawn_render_camera_frozen;
    struct camera local_respawn_render_camera;
    bool host_respawn_loading;
    bool host_respawn_render_camera_frozen;
    struct camera host_respawn_render_camera;
};
static struct {
    struct client_session players[2];
    struct world world;
    bool world_loaded;
    bool world_switch_pending;
    bool world_load_failed;
    bool remote_session;
    bool remote_terrain_hold;
    bool paused;
    bool config_creative;
    struct { float dt; } stats;
} gstate;
static struct { bool enabled; bool split_guest; } hw_run;
/* split_recovery_6m (2026-09-17): the extracted seam reads the driver's action
 * struct and armed flag. All-zero / false = disarmed, and the seam must behave
 * exactly as before -- every assertion below runs against that. */
static struct { bool suppress_autojoin, drive_guest_walk, drive_guest_body,
    guest_frame_valid, host_frame_valid;
    struct input_player_frame guest_frame, host_frame; } s_split_recovery_act;
static bool split_recovery_armed(void) { return false; }
/* host_vehicle_smoke_8m (vehicle-smoke-1004): the extracted seam also reads
 * the vehicle driver's action struct. All-zero = disarmed, as above. */
static struct { bool suppress_autojoin, guest_frame_valid, guest_look;
    float guest_yaw, guest_pitch;
    struct input_player_frame guest_frame; } s_vehicle_act;
static bool hw_profile_done_sent;
static bool fixture_payloads, fixture_guest_held, fixture_host_transfer_episode;
static uint32_t fixture_guest_request_id;
static int fixture_guest_view_slot;
static struct client_ctx_registry fixture_registry;
static struct client_world_context fixture_contexts[CLIENT_CTX_SLOT_COUNT];
static struct world fixture_slot_worlds[CLIENT_CTX_SLOT_COUNT];
static bool fixture_context_present[CLIENT_CTX_SLOT_COUNT];
static bool fixture_slot_ready[CLIENT_CTX_SLOT_COUNT];
static bool fixture_slot_enter_ret, fixture_handoff_ret;
static unsigned slot_enter_calls, slot_leave_calls, handoff_calls;
static unsigned slot_enter_arg, handoff_slot_arg;
static uint32_t handoff_generation_arg;
static int handoff_dimension_arg;
bool clin_world_payloads_enabled(void) { return fixture_payloads; }
uint32_t clin_transfer_actor_request_id(unsigned p) {
    assert(p == 1); return fixture_guest_request_id;
}
bool clin_transfer_actor_held(unsigned p) {
    assert(p == 1); return fixture_guest_held;
}
bool clin_host_transfer_pane_active(void) { return fixture_host_transfer_episode; }
int clin_world_view_slot(unsigned p) { assert(p == 1); return fixture_guest_view_slot; }
struct client_ctx_registry* clin_world_contexts(void) { return &fixture_registry; }
struct client_world_context* client_ctx_registry_current(
        struct client_ctx_registry* registry, unsigned slot) {
    assert(registry == &fixture_registry);
    return slot < CLIENT_CTX_SLOT_COUNT && fixture_context_present[slot]
        ? &fixture_contexts[slot] : NULL;
}
bool clin_world_payload_ready(unsigned s) {
    assert(s < CLIENT_CTX_SLOT_COUNT); return fixture_slot_ready[s];
}
struct clin_view_world { struct world* terrain; };
bool clin_world_slot_enter(unsigned slot, struct clin_view_world* v) {
    assert(slot < CLIENT_CTX_SLOT_COUNT && v);
    slot_enter_calls++; slot_enter_arg=slot;
    if(!fixture_slot_enter_ret) return false;
    v->terrain=&fixture_slot_worlds[slot]; return true;
}
bool clin_guest_respawn_handoff(unsigned slot, uint32_t generation,
        int dimension) {
    handoff_calls++; handoff_slot_arg=slot;
    handoff_generation_arg=generation; handoff_dimension_arg=dimension;
    return fixture_handoff_ret;
}
bool clin_world_view_enter(unsigned p, struct clin_view_world* v) {
    assert(p == 1); v->terrain = &gstate.world; return true;
}
void clin_world_view_leave(struct clin_view_world* v) {
    assert(v && v->terrain); slot_leave_calls++;
}

/* ---- main.c's own loop locals, as file-scope statics ------------------- */
static bool split_experimental;
static bool split_join_settled;
static bool split_guest_logged;
static bool split_final_logged;
static bool split_world_was_loaded;

/* LOADING_POLICY (sliced from source/network/client_frame_worlds.c) */
/* LOADING_POLICY */

/* ---- recorded effects --------------------------------------------------- */
#define LOG_MAX 32
static const char* events[LOG_MAX];
static unsigned events_n;
static void note(const char* what) {
    assert(events_n < LOG_MAX);
    events[events_n++] = what;
}
static void log_reset(void) { events_n = 0; }
static bool log_is(const char* a, const char* b, const char* c) {
    const char* want[3] = {a, b, c};
    unsigned n = 0;
    while(n < 3 && want[n]) n++;
    if(events_n != n) return false;
    for(unsigned i = 0; i < n; i++)
        if(strcmp(events[i], want[i])) return false;
    return true;
}
static void log_dump(const char* label) {
    fprintf(stderr, "FAIL %s: effects were [", label);
    for(unsigned i = 0; i < events_n; i++)
        fprintf(stderr, "%s%s", i ? ", " : "", events[i]);
    fprintf(stderr, "]\n");
}
#define EXPECT(label, a, b, c) do { \
        if(!log_is(a, b, c)) { log_dump(label); exit(1); } \
    } while(0)

static bool claimed_ret;
static bool admit_failure_ret; /* admit-retry-0925: one unreported retirement */
/* admit-retry-0925: when >= 0 the join stub moves the guest to this phase, as
 * the real request does (a Retry re-sends the Leave: FAILED -> LEAVING). */
static int join_sets_phase = -1;
static bool maintain_ret, refuse_ret;
static bool split_off_blocks_ret;
static struct client_session* readiness_session_arg;
static struct world* readiness_world_arg;
static bool readiness_world_ready_arg, readiness_world_failed_arg;
static bool readiness_terrain_hold_arg;
static bool readiness_ret;
static bool maintain_session_arg, maintain_admitted_arg;
static bool refuse_joining_arg, refuse_admitted_arg;
static unsigned readiness_calls, maintain_calls, refuse_calls, flushes;
static unsigned split_off_blocks_calls, join_arm_calls;
static bool join_arm_value;
static bool lworld_admit_ret;
static bool inventory_pressed;
static bool split_container_ret;
static bool hidden_camera_ret;
static bool host_hidden_camera_ret;
static unsigned hidden_camera_calls;
static const struct client_session* hidden_camera_owner;
static bool lod_guest_selected_hidden;
static bool lod_guest_render_safe;
static int fixture_gfx_height = 480;

int gfx_height(void) { return fixture_gfx_height; }
int gfx_width(void) { return 640; }
bool lworld_admit_gameplay(void) { return lworld_admit_ret; }
bool local_menu_host_active(const struct client_session* s) {
    return s->menu_host.active;
}
bool local_pause_active(const struct local_pause* p) { return p->active; }
/* b1-chat-opener-0919/07 C: the creative picker's gameplay OPEN moved off the
 * menu Close role onto IB_OPEN_INVENTORY. The stub keeps asserting that the
 * extracted seam reads exactly ONE role, and names the one it must be -- so a
 * future slide back onto the Close role fails here by name. */
enum { IB_INVENTORY = 0, IB_OPEN_INVENTORY = 1, WINDOWC_INVENTORY = 1 };
bool input_pressed(int button) {
    assert(button == IB_OPEN_INVENTORY
           && "the creative-picker seam must read the gameplay OPEN role");
    return inventory_pressed;
}
bool screen_split_container_base(uint8_t window, int width, int height) {
    (void)window; (void)width; (void)height;
    return split_container_ret;
}
void local_menu_host_reset(struct client_session* s) {
    s->menu_host.active = false;
}
bool local_menu_host_open_creative(struct client_session* s,unsigned p,int w,int h) {
    assert(p==0 && w==640 && h==240);
    s->menu_host.active=s->menu_host.split_owned=true;
    s->menu_host.mode=LOCAL_MENU_CREATIVE;return true;
}
static unsigned picker_compacted, picker_expanded;
void screen_creative_picker_to_compact(struct creative_picker* p) {
    p->fixture_selection=17;picker_compacted++;
}
void screen_creative_picker_from_compact(const struct creative_picker* p) {
    assert(p->fixture_selection==17);picker_expanded++;
}
void input_pointer_enable(bool enabled) {(void)enabled;}
bool clin_show_window(uint8_t window) { (void)window; return false; }
bool client_session_local_respawn_hidden_camera(
        const struct client_session* owner, struct camera* out) {
    hidden_camera_calls++;
    hidden_camera_owner = owner;
    if(!hidden_camera_ret) return false;
    *out = owner->camera;
    out->fixture_identity = 91;
    return true;
}
struct camera* client_session_local_render_camera(struct client_session* s) {
    if(s->host_respawn_render_camera_frozen) return &s->host_respawn_render_camera;
    return s->local_respawn_render_camera_frozen
        ? &s->local_respawn_render_camera : &s->camera;
}
bool client_session_host_gameplay_active(const struct client_session* s) {
    return s && s->local_player && s->local_player->health>0
        && !s->host_respawn_loading && !s->death.active;
}
bool client_session_local_gameplay_active(const struct client_session* s) {
    return s && s->local_bound && s->local_generation
        && s->local_phase==CLIENT_GUEST_ATTACHED && s->local_player
        && s->local_player->health>0 && s->local_gameplay_ready
        && !s->local_respawn_loading && !s->death.active;
}
bool client_session_host_respawn_hidden_camera(
        const struct client_session* owner, struct camera* out) {
    if(!host_hidden_camera_ret) return false;
    *out=owner->camera; out->fixture_identity=90; return true;
}
bool client_session_host_respawn_readiness(struct client_session* s,
        struct world* w,bool ready,bool failed,bool hold) {
    (void)s;(void)w;(void)ready;(void)failed;(void)hold; return false;
}
void client_session_flush_host_respawn(struct client_session* s) {(void)s;}

static bool recovery_ret;
static unsigned death_updates, other_draws;
static int other_draw_owner=-1;
bool local_menu_host_recovery_frame(struct client_session* s,unsigned p,
 const struct input_player_frame* f,int x,int y,int w,int h) {
 (void)s;(void)p;(void)f;(void)x;(void)y;(void)w;(void)h; return recovery_ret;
}
void screen_death_update_viewport(float dt,int w,int h) {
 (void)dt;(void)w;(void)h; death_updates|=1;
}
bool screen_death_owner_update(struct client_session* s,unsigned p,
 const struct input_player_frame* f,float dt,int x,int y,int w,int h) {
 (void)s;(void)f;(void)dt;(void)x;(void)y;(void)w;(void)h;
 death_updates|=1u<<p;return true;
}
void entity_local_player_render_other(struct client_session* s,
 const float* view,float dt) {
 (void)view;(void)dt;other_draws++;other_draw_owner=(int)(s-gstate.players);
}
/* lava-aabb-0924/B2+B3: the partner-body fire hook in
 * local_render_other_player_draw; presentation only, not part of the seam. */
bool clin_local_body_fire_visible(const struct client_session* s) {
 (void)s;return false;
}
void render_entity_fire_draw(const struct entity* e,const struct camera* c,
 float dt) {
 (void)e;(void)c;(void)dt;
}
static unsigned render3d_calls[2];
void gfx_fog(bool on) { (void)on; }
enum { WDB_SCREEN3D };
void watchdog_draw_breadcrumb(int phase, int a, int b) {
    assert(phase == WDB_SCREEN3D); (void)a; (void)b;
}
void screen_ingame_render3D_owned(struct client_session* owner,
        const float* view) {
    (void)view; render3d_calls[owner - gstate.players]++;
}
static void fixture_render3d(struct screen* screen, const float* view) {
    (void)screen; (void)view; render3d_calls[0]++;
}

bool client_session_local_respawn_readiness(struct client_session* session,
        struct world* world, bool world_ready, bool world_failed,
        bool terrain_hold) {
    readiness_calls++;
    readiness_session_arg = session;
    readiness_world_arg = world;
    readiness_world_ready_arg = world_ready;
    readiness_world_failed_arg = world_failed;
    readiness_terrain_hold_arg = terrain_hold;
    return readiness_ret;
}

bool input_split_join_claimed(void) { return claimed_ret; }
void input_split_join_arm(bool armed) {
    join_arm_calls++;
    join_arm_value = armed;
}
void input_split_join_settle(bool joined) {
    note(joined ? "settle(true)" : "settle(false)");
}
bool screen_options_split_guest_maintain(bool session_exists, bool admitted) {
    maintain_calls++;
    maintain_session_arg = session_exists;
    maintain_admitted_arg = admitted;
    return maintain_ret;
}
bool screen_options_split_guest_refuse(bool joining, bool admitted) {
    refuse_calls++;
    refuse_joining_arg = joining;
    refuse_admitted_arg = admitted;
    return refuse_ret;
}
bool screen_options_split_off_blocks_join(void) {
    split_off_blocks_calls++;
    return split_off_blocks_ret;
}
bool client_session_local_admit_failure_present(struct client_session* g,
                                                struct client_session* h) {
    assert(g == &gstate.players[1] && h == &gstate.players[0]);
    if(!admit_failure_ret) return false;
    admit_failure_ret = false; /* the real helper reports once */
    note("admit_failure");
    return true;
}
bool client_session_request_local_join(struct client_session* s) {
    assert(s == &gstate.players[1]);
    note("join");
    if(join_sets_phase >= 0) s->local_phase = join_sets_phase;
    return true;
}
bool client_session_request_local_leave(struct client_session* s) {
    assert(s == &gstate.players[1]);
    note("leave");
    return true;
}
bool client_session_request_local_gameplay(struct client_session* s) {
    assert(s == &gstate.players[1]);
    note("gameplay");
    return true;
}
void client_session_flush_local(struct client_session* s) {
    assert(s == &gstate.players[1]);
}
bool screen_options_split_flush(void) { flushes++; return true; }

/* ---- the production seams, verbatim ------------------------------------ */
/* MAIN_LOCAL_CONTAINER_VIEWS */
static bool death_view_now(const struct client_session* owner,unsigned player) {
    struct clin_frame_loading_policy loading=local_frame_loading_policy();
    return local_death_view(owner,player,&loading);
}

static unsigned lod_frame(unsigned view_count, struct camera** selected,
        struct camera* hidden) {
/* MAIN_RENDER_CAMERAS */
    struct camera* lod_cameras[2] = {render_cameras[0], render_cameras[1]};
    unsigned lod_view_count = view_count;
    struct camera hidden_respawn_camera[2];
/* MAIN_RESPAWN_LOD_BLOCK */
    lod_guest_selected_hidden = lod_cameras[1] == &hidden_respawn_camera[1];
    lod_guest_render_safe = render_cameras[1] != &hidden_respawn_camera[1];
    selected[0] = lod_cameras[0];
    selected[1] = render_cameras[1];
    *hidden = hidden_respawn_camera[1];
    return lod_view_count;
}

static void guest_frame(void) {
    log_reset();
    readiness_calls = 0;
    readiness_session_arg = NULL;
    readiness_world_arg = NULL;
    readiness_world_ready_arg = false;
    readiness_world_failed_arg = false;
    readiness_terrain_hold_arg = false;
    maintain_calls = refuse_calls = 0;
    maintain_session_arg = maintain_admitted_arg = false;
    refuse_joining_arg = refuse_admitted_arg = false;
/* MAIN_GUEST_BLOCK */
}

static void render3d_frame(unsigned viewport, unsigned view_count) {
    struct client_session* owner = &gstate.players[viewport];
    struct camera* render_camera = &owner->camera;
/* MAIN_RENDER3D_BLOCK */
}

static void reset_world(void);

/* A portal transfer clears guest gameplay readiness while retaining its pane.
 * Run main.c's maintenance, viewport policy and actual draw block in order. */
static void s7_guest_loading_null_screen_render(void) {
    reset_world();
    fixture_payloads = true;
    fixture_guest_view_slot = 0;
    gstate.players[0].current_screen = &screen_ingame;
    struct client_session* guest = &gstate.players[1];
    guest->local_bound = true;
    guest->local_ticket = 41;
    guest->local_generation = 17;
    guest->local_phase = CLIENT_GUEST_ATTACHED;
    guest->local_gameplay_ready = true;
    maintain_ret = true;
    guest_frame();
    assert(guest->current_screen == &screen_ingame);
    fixture_guest_request_id = 1;
    fixture_guest_held = true;
    local_transfer_panes_update();
    guest->local_gameplay_ready = false;
    guest_frame();
    assert(guest->current_screen == NULL && s_guest_transfer_pane);
    bool converted = true;
    unsigned views = local_container_views(&converted, false);
    assert(views == 2 && !converted);
    render3d_frame(0, views);
    assert(render3d_calls[0] == 1 && render3d_calls[1] == 0);
    puts("PROBE (7) guest loading pane has NULL screen; entering production render3D block");
    render3d_frame(1, views); /* old main.c dereferenced NULL here */
    assert(render3d_calls[0] == 1 && render3d_calls[1] == 0);
    fixture_guest_held = false;
    guest->local_gameplay_ready = true;
    guest_frame();
    assert(guest->current_screen == &screen_ingame);
    render3d_frame(1, views);
    assert(render3d_calls[0] == 1 && render3d_calls[1] == 1);
    puts("PASS (7) retained two-pane guest loading skips NULL screen 3D; host and restored guest render");
}

static void join_arm_frame(void) {
/* MAIN_JOIN_ARM */
}

static void unload_frame(void) {
/* MAIN_UNLOAD_FLUSH */
}

/* S3: the sliced shipping block must poll the guest's real readiness helper
 * once per frame, before any optional guest-maintenance body. Pin the exact
 * owner/world addresses and all three gates so adding a compile-only no-op
 * declaration cannot hide a missing or miswired call. */
static void s0_respawn_readiness_invocation(void) {
    reset_world();
    maintain_ret = false; /* proves readiness is outside/above maintenance */
    gstate.world.fixture_identity = 73;
    guest_frame();
    assert(readiness_calls == 1);
    assert(readiness_session_arg == &gstate.players[1]);
    assert(readiness_world_arg == &gstate.world);
    assert(readiness_world_arg->fixture_identity == 73);
    assert(readiness_world_ready_arg);
    assert(!readiness_world_failed_arg && !readiness_terrain_hold_arg);

    /* The settled-world argument is composed here in main; isolate that
     * expression from the two independent status flags. */
    gstate.world_switch_pending = true;
    guest_frame();
    assert(readiness_calls == 1 && !readiness_world_ready_arg);
    assert(!readiness_world_failed_arg && !readiness_terrain_hold_arg);

    /* Keep the world ready and raise each remaining gate alone. Swapping or
     * collapsing the last two arguments must fail one of these cases. */
    gstate.world_switch_pending = false;
    gstate.world_load_failed = true;
    guest_frame();
    assert(readiness_calls == 1);
    assert(readiness_session_arg == &gstate.players[1]);
    assert(readiness_world_arg == &gstate.world);
    assert(readiness_world_ready_arg);
    assert(readiness_world_failed_arg && !readiness_terrain_hold_arg);

    gstate.world_load_failed = false;
    gstate.remote_terrain_hold = true;
    guest_frame();
    assert(readiness_calls == 1);
    assert(readiness_session_arg == &gstate.players[1]);
    assert(readiness_world_arg == &gstate.world);
    assert(readiness_world_ready_arg);
    assert(!readiness_world_failed_arg && readiness_terrain_hold_arg);

    gstate.remote_terrain_hold = false;
    gstate.world_loaded = false;
    guest_frame();
    assert(readiness_calls == 1 && !readiness_world_ready_arg);
    puts("PASS (0) respawn readiness invocation: owner 1 and the shared world "
         "are polled once even without maintenance, settled-world is composed "
         "from loaded&&!switch_pending, and failure/terrain gates pass through");
}

/* The same extracted block must use the respawn's authoritative destination,
 * even when guest view 1 is still bound to the old world. No stale context may
 * admit READY, and the world selection bracket must balance on success. */
static void s0b_respawn_target_identity(void) {
    reset_world();
    fixture_payloads=true;
    fixture_guest_view_slot=0; /* deliberately not the target */
    struct client_session* guest=&gstate.players[1];
    guest->local_respawn_loading=true;
    guest->local_respawn_world_slot=1;
    guest->local_respawn_world_generation=23;
    guest->local_respawn_dimension=-1;
    guest->local_respawn_ack_send_pending=true;
    fixture_context_present[1]=true;
    fixture_contexts[1].server_generation=23;
    fixture_contexts[1].dimension=-1;
    fixture_slot_ready[1]=true;
    fixture_slot_worlds[1].fixture_identity=117;
    maintain_ret=false; /* target check precedes optional maintenance */
    gstate.world_loaded=false;
    gstate.world_switch_pending=true;
    guest_frame();
    assert(readiness_calls==1 && readiness_session_arg==guest);
    assert(readiness_world_arg==&fixture_slot_worlds[1]
        && readiness_world_arg->fixture_identity==117);
    assert(readiness_world_ready_arg && !readiness_world_failed_arg
        && !readiness_terrain_hold_arg);
    assert(slot_enter_calls==1 && slot_enter_arg==1 && slot_leave_calls==1);
    assert(handoff_calls==0 && guest->local_respawn_ack_send_pending);

    readiness_ret=true;
    fixture_handoff_ret=true;
    slot_enter_calls=slot_leave_calls=handoff_calls=0;
    guest_frame();
    assert(readiness_calls==1 && slot_enter_calls==1 && slot_leave_calls==1);
    assert(handoff_calls==1 && handoff_slot_arg==1
        && handoff_generation_arg==23 && handoff_dimension_arg==-1);
    assert(guest->local_respawn_ack_send_pending);

    /* A rejected handoff leaves no accepted envelope; the production block
     * clears the pending-send flag only on this path. */
    fixture_handoff_ret=false;
    slot_enter_calls=slot_leave_calls=handoff_calls=0;
    guest_frame();
    assert(readiness_calls==1 && handoff_calls==1
        && slot_enter_calls==1 && slot_leave_calls==1);
    assert(!guest->local_respawn_ack_send_pending);

    /* Every identity/readiness predicate independently fails closed. */
    guest->local_respawn_loading=false;
    slot_enter_calls=slot_leave_calls=handoff_calls=0;
    guest_frame();
    assert(readiness_calls==0 && slot_enter_calls==0 && handoff_calls==0);
    guest->local_respawn_loading=true;
    guest->local_respawn_world_slot=2;
    guest_frame();
    assert(readiness_calls==0 && slot_enter_calls==0 && handoff_calls==0);
    guest->local_respawn_world_slot=1;
    fixture_context_present[1]=false;
    guest_frame();
    assert(readiness_calls==0 && slot_enter_calls==0 && handoff_calls==0);
    fixture_context_present[1]=true;
    fixture_contexts[1].server_generation=24;
    guest_frame();
    assert(readiness_calls==0 && slot_enter_calls==0 && handoff_calls==0);
    fixture_contexts[1].server_generation=23;
    fixture_contexts[1].dimension=0;
    guest_frame();
    assert(readiness_calls==0 && slot_enter_calls==0 && handoff_calls==0);
    fixture_contexts[1].dimension=-1;
    fixture_slot_ready[1]=false;
    guest_frame();
    assert(readiness_calls==0 && slot_enter_calls==0 && handoff_calls==0);
    fixture_slot_ready[1]=true;
    fixture_slot_enter_ret=false;
    guest_frame();
    assert(readiness_calls==0 && slot_enter_calls==1
        && slot_leave_calls==0 && handoff_calls==0);
    puts("PASS (0b) respawn target uses exact slot, generation and dimension; "
         "stale/unready contexts fail closed, target terrain and handoff "
         "are correct, world selection balances");
}

/* ---- gap 4: main.c's use of the two predicates ------------------------- */
/* The membership main.c computes, stated independently of the slice. A phase
 * the enum gains and this switch does not name fails LOUDLY rather than being
 * classified by omission. */
static bool expect_joining(int phase) {
    switch(phase) {
        case CLIENT_GUEST_REQUESTED:
        case CLIENT_GUEST_INSTALLING:
        case CLIENT_GUEST_CONFIRMING:
        case CLIENT_GUEST_ATTACHED:
            return true;
        case CLIENT_GUEST_IDLE:
        case CLIENT_GUEST_LEAVING:
        case CLIENT_GUEST_FAILED:
            return false;
    }
    fprintf(stderr, "FAIL: client_guest_phase %d is unclassified -- decide "
        "whether main.c's guest_joining set includes it\n", phase);
    exit(1);
}

static void reset_world(void) {
    static struct entity host, guest_body;
    memset(&gstate, 0, sizeof(gstate));
    gstate.players[0].local_player = &host;
    gstate.players[1].local_player = &guest_body;
    host.health = guest_body.health = 20;
    gstate.world_loaded = true;
    gstate.world_switch_pending = false;
    fixture_payloads = fixture_guest_held = fixture_host_transfer_episode = false;
    fixture_guest_request_id = 0;
    fixture_guest_view_slot = -1;
    memset(fixture_contexts, 0, sizeof fixture_contexts);
    memset(fixture_slot_worlds, 0, sizeof fixture_slot_worlds);
    memset(fixture_context_present, 0, sizeof fixture_context_present);
    memset(fixture_slot_ready, 0, sizeof fixture_slot_ready);
    fixture_slot_enter_ret = true;
    fixture_handoff_ret = false;
    slot_enter_calls = slot_leave_calls = handoff_calls = 0;
    slot_enter_arg = handoff_slot_arg = 0;
    handoff_generation_arg = 0;
    handoff_dimension_arg = 0;
    readiness_ret = false;
    s_guest_transfer_pane = false;
    s_guest_transfer_generation = s_guest_transfer_request_seen = 0;
    screen_ingame.render3D = fixture_render3d;
    render3d_calls[0] = render3d_calls[1] = 0;
    hw_run.enabled = false;
    hw_run.split_guest = false;
    hw_profile_done_sent = false;
    split_experimental = false;
    split_join_settled = false;
    split_guest_logged = false;
    split_final_logged = false;
    claimed_ret = false;
    admit_failure_ret = false;
    join_sets_phase = -1;
    maintain_ret = false;
    refuse_ret = false;
    split_off_blocks_ret = false;
    split_off_blocks_calls = 0;
    join_arm_calls = 0;
    join_arm_value = false;
    lworld_admit_ret = true;
    inventory_pressed = false;
    split_container_ret = false;
    hidden_camera_ret = false;
    host_hidden_camera_ret = false;
    hidden_camera_calls = 0;
    hidden_camera_owner = NULL;
    lod_guest_selected_hidden = false;
    lod_guest_render_safe = false;
    recovery_ret=false;death_updates=other_draws=0;other_draw_owner=-1;
    fixture_gfx_height = 480;
}

/* S2 golden: this calls main.c's complete local_container_views function,
 * sliced above. No view-count predicate is copied into the fixture. */
static void s6_respawn_viewport_continuity(void) {
    bool converted = true;
    reset_world();
    gstate.players[0].current_screen = &screen_ingame;
    assert(local_container_views(&converted, false) == 1 && !converted);

    converted = true;
    gstate.players[1].local_bound = true;
    gstate.players[1].local_gameplay_ready = true;
    assert(local_container_views(&converted, false) == 2 && !converted);

    /* Creative open remains two owner-local panes, just like inventory. */
    gstate.config_creative = true;
    inventory_pressed = true;
    assert(local_container_views(&converted, false) == 2 && !converted);
    inventory_pressed = false;
    gstate.config_creative = false;

    gstate.config_creative = true;
    gstate.players[0].current_screen = &screen_creative_picker;
    fixture_gfx_height = 576; /* menu still uses the centered 240px band */
    assert(local_container_views(&converted, false)==2 && converted);
    assert(picker_compacted==1 && gstate.players[0].current_screen==&screen_ingame);
    assert(gstate.players[0].menu_host.active);
    gstate.players[1].local_bound=false;
    assert(local_container_views(&converted, false)==1 && converted);
    assert(picker_expanded==1 && gstate.players[0].current_screen==&screen_creative_picker);
    assert(!gstate.players[0].menu_host.active);
    fixture_gfx_height = 480;
    gstate.players[0].current_screen=&screen_ingame;
    gstate.players[1].local_bound=true;
    gstate.config_creative=false;

    /* Existing global transition remains one presentation rectangle. */
    converted = true;
    gstate.world_switch_pending = true;
    lworld_admit_ret = false;
    assert(local_container_views(&converted, true) == 1 && !converted);
    puts("PASS (6a) actual local_container_views baseline: SP=1, admitted split=2, global transition=1");

    /* S3 LOADING deliberately makes gameplay inert, but ownership and the
     * retained body still owe the same presentation slot. Keeping count=2
     * preserves owner 0 as rectangle 0 and owner 1 as rectangle 1; count=1
     * would resize the surviving owner 0 even though no one left. */
    reset_world();
    gstate.players[0].current_screen = &screen_ingame;
    struct client_session* survivor = &gstate.players[0];
    struct client_session* respawning = &gstate.players[1];
    struct entity* retained_body = respawning->local_player;
    respawning->local_bound = true;
    respawning->local_ticket = 41;
    respawning->local_generation = 17;
    respawning->local_phase = CLIENT_GUEST_ATTACHED;
    respawning->local_gameplay_ready = false;
    respawning->local_respawn_loading = true;
    respawning->local_respawn_epoch = 9;
    respawning->local_respawn_completed_epoch = 8;
    respawning->local_player->data.local_player.capture_input = false;
    converted = true;
    unsigned views = local_container_views(&converted, false);
    assert(&gstate.players[0] == survivor && &gstate.players[1] == respawning);
    assert(respawning->local_player == retained_body
        && !respawning->local_gameplay_ready
        && !respawning->local_player->data.local_player.capture_input);
    assert(!converted);
    if(views != 2) {
        fprintf(stderr, "FAIL S2 viewport continuity: retained owner-1 "
            "respawn LOADING returned %u view; expected 2 so survivor owner-0 "
            "keeps rectangle 0 while owner-1 gameplay/input/physics stay inert\n",
            views);
        exit(1);
    }
    puts("PASS (6b) retained respawn LOADING keeps two owner rectangles while gameplay stays inert");

    /* The presentation predicate must not admit incomplete, released or stale
     * tuples merely because a retained body pointer exists. */
    respawning->local_phase = CLIENT_GUEST_LEAVING;
    assert(local_container_views(&converted, false) == 1);
    respawning->local_phase = CLIENT_GUEST_ATTACHED;
    respawning->local_respawn_epoch = respawning->local_respawn_completed_epoch;
    assert(local_container_views(&converted, false) == 1);
    respawning->local_respawn_epoch = 9;
    respawning->local_bound = false;
    assert(local_container_views(&converted, false) == 1);
    respawning->local_bound = true;
    respawning->local_player = NULL;
    assert(local_container_views(&converted, false) == 1);
    respawning->local_player = retained_body;

    /* Presentation=2 must not change the actual gameplay seam: owner 1 stays
     * ineligible for both its physics tick and capture assignment. */
    assert(!local_session_gameplay_active(respawning, 1));
    assert(local_session_gameplay_active(survivor, 0));
    assert(!respawning->local_player->data.local_player.capture_input);
    assert(!death_view_now(survivor, 0));
    respawning->local_player->health = 0;
    assert(death_view_now(respawning, 1));
    respawning->local_player->health = 20;
    respawning->death.active = true;
    assert(death_view_now(respawning, 1));
    gstate.world_switch_pending = true;
    assert(!death_view_now(respawning, 1));
    gstate.world_switch_pending = false;
    respawning->death.active = false;

    /* Owner 0 remains visible from owner 1; an unready owner 1 is hidden
     * from owner 0 until the same gameplay-ready gate opens. */
    assert(local_render_other_player(1));
    assert(!local_render_other_player(0));
    respawning->local_gameplay_ready = true;
    assert(!local_render_other_player(0)); /* READY cannot bypass LOADING */
    respawning->local_respawn_loading=false;
    assert(local_render_other_player(0));
    respawning->local_respawn_loading=true;
    respawning->local_gameplay_ready = false;

    /* With two presentation rectangles, residency still follows the hidden
     * destination camera. The retained render camera remains the safe old
     * body camera and is never replaced by that target. */
    struct camera* selected[2] = {0};
    struct camera hidden = {0};
    respawning->camera.fixture_identity = 37;
    respawning->local_respawn_render_camera.fixture_identity = 38;
    respawning->local_respawn_render_camera_frozen = true;
    hidden_camera_ret = true;
    unsigned lod_views = lod_frame(2, selected, &hidden);
    assert(hidden_camera_calls == 1 && hidden_camera_owner == respawning);
    assert(lod_views == 2 && selected[0] == &survivor->camera);
    assert(lod_guest_selected_hidden && lod_guest_render_safe
        && selected[1] == &respawning->local_respawn_render_camera
        && hidden.fixture_identity == 91
        && respawning->camera.fixture_identity == 37);
    puts("PASS (6c) retained camera renders safely while two-view LOD uses the hidden destination; survivor remains gameplay-active");

    other_draws=0;
    local_render_other_player_draw(0,&respawning->camera,0);
    assert(!other_draws);
    respawning->local_gameplay_ready=true;
    respawning->local_respawn_loading=false;
    local_render_other_player_draw(0,&survivor->camera,0);
    assert(other_draws==1&&other_draw_owner==1);
    survivor->host_respawn_loading=true;
    local_render_other_player_draw(1,&respawning->camera,0);
    assert(other_draws==1);
    survivor->host_respawn_loading=false;
    local_render_other_player_draw(1,&respawning->camera,0);
    assert(other_draws==2&&other_draw_owner==0);

    struct split_layout_rect rect={.y=240,.height=240};
    struct input_player_frame frame={.connected=true};
    bool pointer=false;
    survivor->death.active=true;
    survivor->local_player->data.local_player.capture_input=true;
    assert(local_death_arbitrate(survivor,0,&frame,rect,true,&pointer));
    assert(pointer&&death_updates==1
        &&!survivor->local_player->data.local_player.capture_input);
    death_updates=0;pointer=false;survivor->menu_host.active=true;
    assert(!local_death_arbitrate(survivor,0,&frame,rect,true,&pointer)
        &&!death_updates);
    survivor->menu_host.active=false;recovery_ret=true;
    assert(local_death_arbitrate(survivor,0,&frame,rect,true,&pointer)
        &&!death_updates);
    puts("PASS (6d) actual arbitration and cross-body draw call routing preserve owner isolation");
}

/* (1) The arguments: `session_exists` is "not IDLE", `admitted` is the body,
 * and `joining` is the TRYING set -- LEAVING deliberately excluded, which is
 * what keeps a refused guest from being re-refused (and re-LEFT) every frame.
 * FAILED is excluded too: it has no window to hand back. */
static void s1_membership(void) {
    for(int phase = CLIENT_GUEST_IDLE; phase <= CLIENT_GUEST_FAILED; phase++) {
        for(int admitted = 0; admitted < 2; admitted++) {
            reset_world();
            maintain_ret = true; /* let the block run so refuse() is reached */
            gstate.players[1].local_phase = phase;
            gstate.players[1].local_gameplay_ready = admitted != 0;
            guest_frame();
            assert(maintain_calls == 1 && refuse_calls == 1);
            if(maintain_session_arg != (phase != CLIENT_GUEST_IDLE)
               || maintain_admitted_arg != (admitted != 0)
               || refuse_joining_arg != expect_joining(phase)
               || refuse_admitted_arg != (admitted != 0)) {
                fprintf(stderr, "FAIL membership: phase=%d admitted=%d -> "
                    "maintain(%d,%d) refuse(%d,%d)\n", phase, admitted,
                    maintain_session_arg, maintain_admitted_arg,
                    refuse_joining_arg, refuse_admitted_arg);
                exit(1);
            }
        }
    }
    /* ...and with the block switched off the predicate still decides alone: no
     * refuse() is even asked, and nothing is settled or requested. */
    reset_world();
    maintain_ret = false;
    gstate.players[1].local_phase = CLIENT_GUEST_ATTACHED;
    guest_frame();
    assert(maintain_calls == 1 && refuse_calls == 0);
    EXPECT("maintain=false", NULL, NULL, NULL);
    puts("PASS (1) main.c's predicate arguments: session_exists = phase != IDLE, "
         "admitted = the body, joining = REQUESTED/INSTALLING/CONFIRMING/"
         "ATTACHED only (LEAVING and FAILED excluded), and maintain=false asks "
         "nothing further");
}

/* (2) The refuse SEQUENCE, in order: the card is settled as refused exactly
 * once (the one-settle latch), and only then is the leave requested. No join
 * (the switch is off) and no gameplay re-ask (the else-if arm). */
static void s2_refuse_sequence(void) {
    reset_world();
    maintain_ret = true;
    refuse_ret = true;
    claimed_ret = true; /* the controller is still claimed at this point */
    gstate.players[1].local_phase = CLIENT_GUEST_ATTACHED;
    guest_frame();
    EXPECT("refuse", "settle(false)", "leave", NULL);
    assert(split_join_settled);
    /* The same frame again (the latch is set, the phase has not moved yet):
     * the card is NOT retired twice. */
    guest_frame();
    EXPECT("refuse again", "leave", NULL, NULL);
    /* ...and in production the leave moved the phase to LEAVING synchronously,
     * which leaves the joining set -- so the real predicate returns false and
     * the per-frame RPC stops. That is the argument main.c hands it: */
    gstate.players[1].local_phase = CLIENT_GUEST_LEAVING;
    refuse_ret = false; /* what screen_options.c answers for !joining */
    guest_frame();
    assert(!refuse_joining_arg);
    EXPECT("leaving", NULL, NULL, NULL);
    puts("PASS (2) refuse sequence: settle(false) once, THEN the leave, no join "
         "and no gameplay re-ask, and the LEAVING phase the leave installs is "
         "outside the joining set so nothing repeats");
}

/* (3) A9-M4: the one-settle latch is released when the guest is back to IDLE,
 * OUTSIDE the maintain block -- the only placement that works, because the
 * refuse-and-release path ends in IDLE on a frame where the switch is off,
 * there is no session and no body, so maintain() is false and an inside-the-
 * block reset never ran. A latch stuck set meant the NEXT join's approved card
 * was never retired. */
static void s3_latch_reset_outside_the_block(void) {
    reset_world();
    split_join_settled = true;  /* left over from the refused join */
    maintain_ret = false;       /* switch off, no session, no body */
    gstate.players[1].local_phase = CLIENT_GUEST_IDLE;
    guest_frame();
    assert(!split_join_settled);
    assert(maintain_calls == 1 && refuse_calls == 0);
    EXPECT("idle reset", NULL, NULL, NULL);
    /* ...and the release happens BEFORE a new join can be requested, so the
     * very next admission settles its own card. */
    reset_world();
    split_join_settled = true;
    split_experimental = true;
    claimed_ret = true;
    maintain_ret = true;
    gstate.players[1].local_phase = CLIENT_GUEST_IDLE;
    guest_frame();
    assert(!split_join_settled);
    EXPECT("idle then join", "join", NULL, NULL);
    gstate.players[1].local_phase = CLIENT_GUEST_ATTACHED;
    gstate.players[1].local_gameplay_ready = true;
    guest_frame();
    EXPECT("admitted", "settle(true)", NULL, NULL);
    assert(split_join_settled);
    puts("PASS (3) A9-M4 latch reset: an IDLE guest clears the one-settle latch "
         "even with the block skipped, and it clears BEFORE the join request so "
         "the next admission retires its own card");
}

/* (4) The admission path: a NEW join needs the switch ON, ATTACHED-without-a-
 * body keeps re-asking, the body pins the viewport and arms capture_input, and
 * retiring it releases both. */
static void s4_admission(void) {
    reset_world();
    maintain_ret = true;
    claimed_ret = true;
    gstate.players[1].local_phase = CLIENT_GUEST_IDLE;
    guest_frame(); /* switch OFF: no new join */
    EXPECT("off blocks a new join", NULL, NULL, NULL);
    split_experimental = true;
    guest_frame();
    EXPECT("on allows it", "join", NULL, NULL);
    /* A world that is not ready blocks it just as hard. */
    gstate.world_switch_pending = true;
    guest_frame();
    EXPECT("world switching", NULL, NULL, NULL);
    gstate.world_switch_pending = false;

    gstate.players[1].local_phase = CLIENT_GUEST_ATTACHED;
    guest_frame();
    EXPECT("attached without a body", "gameplay", NULL, NULL);
    assert(!gstate.players[1].current_screen);
    assert(!gstate.players[1].local_player->data.local_player.capture_input);

    gstate.players[1].local_gameplay_ready = true;
    guest_frame();
    EXPECT("body admitted", "settle(true)", NULL, NULL);
    assert(gstate.players[1].current_screen == &screen_ingame);
    assert(gstate.players[1].local_player->data.local_player.capture_input);
    assert(split_guest_logged);
    /* One line per EDGE, not per frame. */
    guest_frame();
    EXPECT("still admitted", NULL, NULL, NULL);

    gstate.players[1].local_gameplay_ready = false;
    gstate.players[1].local_phase = CLIENT_GUEST_LEAVING;
    guest_frame();
    assert(!gstate.players[1].current_screen);
    assert(!gstate.players[1].local_player->data.local_player.capture_input);
    assert(!split_guest_logged);

    /* FAILED settles the card as refused exactly once. */
    reset_world();
    maintain_ret = true;
    split_experimental = true;
    gstate.players[1].local_phase = CLIENT_GUEST_FAILED;
    guest_frame();
    EXPECT("failed", "settle(false)", NULL, NULL);
    guest_frame();
    EXPECT("failed again", NULL, NULL, NULL);

    /* admit-retry-0925 (Astra review 3 item 1): the join card Retry after a
     * refused admission Leave is a claim on a FAILED guest; it opens a NEW
     * card, so the latch must re-open and a SECOND refusal (FAILED again)
     * must settle that card too. */
    claimed_ret = true;
    join_sets_phase = CLIENT_GUEST_LEAVING;
    guest_frame();
    EXPECT("retry claim", "join", NULL, NULL);
    claimed_ret = false;
    join_sets_phase = -1;
    guest_frame();
    EXPECT("retry leave in flight", NULL, NULL, NULL);
    gstate.players[1].local_phase = CLIENT_GUEST_FAILED;
    guest_frame();
    EXPECT("second refusal", "settle(false)", NULL, NULL);
    guest_frame();
    EXPECT("second refusal again", NULL, NULL, NULL);

    /* admit-retry-0925 (Astra review A item 2): an admission retirement goes
     * LEAVING -> IDLE, never FAILED. Its one reported edge settles the card
     * as refused once, even with maintenance off, and IDLE re-opens the
     * latch for the next join. */
    reset_world();
    gstate.players[1].local_phase = CLIENT_GUEST_LEAVING;
    admit_failure_ret = true;
    guest_frame();
    EXPECT("admission retired", "admit_failure", "settle(false)", NULL);
    guest_frame();
    EXPECT("admission retired again", NULL, NULL, NULL);
    gstate.players[1].local_phase = CLIENT_GUEST_IDLE;
    maintain_ret = true;
    split_experimental = true;
    claimed_ret = true;
    guest_frame();
    EXPECT("rejoin after retirement", "join", NULL, NULL);

    /* An armed harness run joins with no controller claim at all. */
    reset_world();
    maintain_ret = true;
    split_experimental = true;
    hw_run.enabled = true;
    hw_run.split_guest = true;
    claimed_ret = false;
    gstate.players[1].local_phase = CLIENT_GUEST_IDLE;
    guest_frame();
    EXPECT("harness auto-join", "join", NULL, NULL);
    puts("PASS (4) admission path: OFF and a switching world both block a NEW "
         "join, ATTACHED re-asks for the body, the body pins the viewport and "
         "arms capture_input (one log edge, not one per frame), FAILED settles "
         "once, and an armed harness run joins without a claim");
}

/* The committed split-OFF coordinator must close both ways a new claim can
 * enter: physical arming before input_poll, and a claim already latched (or
 * synthesized by the hardware profile) before the request branch runs. */
static void s4b_split_off_join_exclusion(void) {
    reset_world();
    split_experimental = true;
    join_arm_frame();
    assert(join_arm_calls == 1 && split_off_blocks_calls == 1
        && join_arm_value);
    split_off_blocks_ret = true;
    join_arm_frame();
    assert(join_arm_calls == 2 && split_off_blocks_calls == 2
        && !join_arm_value);

    reset_world();
    maintain_ret = true;
    split_experimental = true;
    claimed_ret = true; /* a physical claim latched before the modal */
    split_off_blocks_ret = true;
    gstate.players[1].local_phase = CLIENT_GUEST_IDLE;
    guest_frame();
    EXPECT("split-off blocks latched claim", NULL, NULL, NULL);
    assert(split_off_blocks_calls == 1);
    split_off_blocks_ret = false;
    guest_frame();
    EXPECT("inactive allows latched claim", "join", NULL, NULL);
    assert(split_off_blocks_calls == 2);

    reset_world();
    maintain_ret = true;
    split_experimental = true;
    hw_run.enabled = true;
    hw_run.split_guest = true;
    claimed_ret = false; /* main synthesizes claimed from world_ready */
    split_off_blocks_ret = true;
    gstate.players[1].local_phase = CLIENT_GUEST_IDLE;
    guest_frame();
    EXPECT("split-off blocks synthetic claim", NULL, NULL, NULL);
    assert(split_off_blocks_calls == 1);
    split_off_blocks_ret = false;
    guest_frame();
    EXPECT("inactive allows synthetic claim", "join", NULL, NULL);
    assert(split_off_blocks_calls == 2);
    puts("PASS (4b) split-OFF join exclusion: physical arm, latched claim and "
         "synthetic autojoin are blocked only while the coordinator is active");
}

/* ---- gap 5: the falling-edge config flush ------------------------------- */
static void s5_unload_flush_edge(void) {
    reset_world();
    split_world_was_loaded = false;
    flushes = 0;
    gstate.players[0].current_screen = &screen_ingame;
    gstate.world_loaded = true;
    unload_frame(); /* rising edge */
    assert(!flushes && split_world_was_loaded);
    unload_frame(); /* still loaded */
    assert(!flushes);
    /* A9-L9: clin_begin_live_stream_recovery's early return clears
     * world_loaded WITHOUT a screen_set, so the edge can fire with
     * screen_ingame still current. The flush is a blocking libfat SD write --
     * exactly what the A8-M6 deferral removed from the gameplay frame -- so it
     * is skipped there, and the key stays owed to the real teardown. */
    gstate.world_loaded = false;
    unload_frame();
    assert(!flushes && !split_world_was_loaded);

    /* The ordinary teardown: the title or a load screen is already installed,
     * and the write happens exactly once -- an EDGE, not a level. */
    gstate.world_loaded = true;
    unload_frame();
    gstate.players[0].current_screen = NULL;
    gstate.world_loaded = false;
    unload_frame();
    assert(flushes == 1);
    unload_frame();
    assert(flushes == 1);
    /* ...and the next world's teardown flushes again. */
    gstate.world_loaded = true;
    unload_frame();
    gstate.world_loaded = false;
    unload_frame();
    assert(flushes == 2);
    puts("PASS (5) unload-flush edge: no write on the rising edge or while "
         "loaded, NONE on a mid-play world_loaded clear with screen_ingame "
         "current (A9-L9), exactly one per real teardown, and one per world");
}


/* Exercise the real local loading callback and main dispatch. screen_set is
 * a readiness-gate probe: this fixture does not claim to test screen.c. */
static float s_time_on_screen;
static unsigned load_attempts, ordinary_updates;
static bool admit_fixture;
static void screen_set(struct screen* s) {
    if(s==&screen_creative_picker){gstate.players[0].current_screen=s;return;}
    assert(s == &screen_ingame);
    load_attempts++;
    if(admit_fixture) gstate.players[0].current_screen = s;
}
static void screen_lworld_remote_update(struct screen* s, float dt) {
    (void)s; (void)dt;
    assert(!"local fixture must not enter remote escape path");
}
/* LOCAL_LOADING_UPDATE */
static void ordinary_update(struct screen* s, float dt) {
    (void)s; (void)dt; ordinary_updates++;
}
static void host_update_frame(bool host_loading_pane, bool consumed,
                              bool recovery_modal, unsigned view_count) {
    bool menu_consumed[2] = {consumed, false};
    /* main.c's achievements save-failure modal flags (B2 R3); no stats save
     * prompt is open in these loading/update scenarios. */
    bool stats_modal[2] = {false, false};
    struct split_layout_rect view_rect[2] = {{0, 240}, {240, 240}};
    /* HOST_UPDATE_DISPATCH */
}
static void s8_host_loading_progress(void) {
    reset_world();
    gstate.remote_session = false;
    gstate.stats.dt = 0.25f;
    screen_load_world.update = screen_lworld_update;
    screen_ingame.update = ordinary_update;
    gstate.players[0].current_screen = &screen_load_world;
    s_time_on_screen = 0; load_attempts = ordinary_updates = 0;
    admit_fixture = false;
    host_update_frame(true, true, false, 2);
    assert(s_time_on_screen == 0.25f && load_attempts == 1);
    assert(gstate.players[0].current_screen == &screen_load_world);
    host_update_frame(true, true, true, 2);
    assert(s_time_on_screen == 0.25f && load_attempts == 1);
    admit_fixture = true;
    host_update_frame(true, true, false, 2);
    assert(s_time_on_screen == 0.5f && load_attempts == 2);
    assert(gstate.players[0].current_screen == &screen_ingame);
    host_update_frame(true, true, false, 2);
    assert(ordinary_updates == 0);
    host_update_frame(false, false, false, 2);
    assert(ordinary_updates == 1);
    puts("PASS (8) host loading update advances despite consumed input; readiness probe, modal and gameplay gates remain intact");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    s0_respawn_readiness_invocation();
    s0b_respawn_target_identity();
    s1_membership();
    s2_refuse_sequence();
    s3_latch_reset_outside_the_block();
    s4_admission();
    s4b_split_off_join_exclusion();
    s5_unload_flush_edge();
    s6_respawn_viewport_continuity();
    s7_guest_loading_null_screen_render();
    s8_host_loading_progress();
    puts("test_split_live_seam: PASS (main.c's guest-maintenance block and "
         "config-flush falling edge, sliced verbatim from source/main.c)");
    return 0;
}
'''


def slice_between(text, start_marker, end_marker, what):
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    body = text[start:end]
    if not body.strip():
        raise SystemExit("empty slice for " + what)
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--without-render-guard", action="store_true",
                        help="mutation probe: restore the pre-fix NULL dereference in the extracted block")
    args = parser.parse_args()
    main_c = (ROOT / "source/main.c").read_text(encoding="utf-8")
    session_h = (ROOT / "source/game/client_session.h").read_text(encoding="utf-8")

    guest_block = slice_between(
        main_c,
        "\t\tstruct client_session* guest = &gstate.players[1];",
        "\n\n\t\t/* Diagnostics follow the real body",
        "guest-maintenance block")
    join_arm = slice_between(
        main_c,
        # G9 moved the arm into g9_frame_input_poll (one indent level).
        "\tinput_split_join_arm(split_experimental && gstate.world_loaded",
        "\n\tg9_capture_main(11, true, 0);",
        "physical split join arm")
    unload_flush_end = "\n\t\tsplit_world_was_loaded = gstate.world_loaded;"
    unload_flush = slice_between(
        main_c,
        "\t\tif(split_world_was_loaded && !gstate.world_loaded",
        unload_flush_end,
        "unload flush") + unload_flush_end
    local_container_views = (
        slice_between(main_c,
            "static bool local_respawn_presentation(const struct client_session* guest) {",
            "\nstatic void local_guest_transfer_loading_render",
            "guest presentation and loading policy")
        + slice_between(main_c,
            "static bool local_render_other_player(unsigned viewport) {",
            "\nstatic bool local_container_send",
            "local container views"))
    loading_h = (ROOT / "source/network/client_frame_worlds.h").read_text(encoding="utf-8")
    loading_c = (ROOT / "source/network/client_frame_worlds.c").read_text(encoding="utf-8")
    loading_types = slice_between(loading_h,
        "struct clin_frame_loading_state {", "\nstruct clin_frame_worlds",
        "loading policy types")
    loading_policy = slice_between(loading_c,
        "struct clin_frame_loading_policy clin_frame_loading_policy(",
        "\nvoid clin_frame_worlds_begin", "loading policy")
    render3d = slice_between(main_c,
        "\t\t\t\tif(owner->current_screen",
        "\n\n\t\t\t\tif(render_view) {", "render3D dispatch")
    if args.without_render_guard:
        render3d = render3d.replace(
            "if(owner->current_screen && owner->current_screen->render3D) {",
            "if(owner->current_screen->render3D) {", 1)
    respawn_lod = slice_between(
        main_c,
        "\t\tif(client_session_host_respawn_hidden_camera(",
        "\n\t\tif(client_session_host_gameplay_active",
        "respawn LOD camera selection")
    render_cameras = slice_between(
        main_c,
        "\t\tstruct camera* render_cameras[2] = {",
        "\n\t\tstruct camera hidden_respawn_camera[2];",
        "safe render camera selection")
    phase_enum = slice_between(
        session_h, "enum client_guest_phase {", "\n\n", "guest phase enum")

    load_c = (ROOT / "source/game/gui/screen_load_world.c").read_text(encoding="utf-8")
    local_load = slice_between(load_c,
        "static void screen_lworld_update(struct screen* s, float dt) {",
        "\n/* Preparing the view", "local loading update")
    host_update = slice_between(main_c,
        "  if(!recovery_modal&&!menu_consumed[0]&&view_count==2&&gstate.players[0].current_screen==&screen_death)",
        "\n\n\t\t/* mp_chat_login_3m", "host screen update dispatch")

    fixture_text = (HARNESS
        .replace("/* GUEST_PHASE_ENUM */", phase_enum)
        .replace("/* LOADING_TYPES */", loading_types)
        .replace("/* LOCAL_LOADING_UPDATE */", local_load)
        .replace("/* HOST_UPDATE_DISPATCH */", host_update)
        .replace("/* LOADING_POLICY */", loading_policy)
        .replace("/* MAIN_LOCAL_CONTAINER_VIEWS */", local_container_views)
        .replace("/* MAIN_RENDER3D_BLOCK */", render3d)
        .replace("/* MAIN_RENDER_CAMERAS */", render_cameras)
        .replace("/* MAIN_RESPAWN_LOD_BLOCK */", respawn_lod)
        .replace("/* MAIN_GUEST_BLOCK */", guest_block)
        .replace("/* MAIN_JOIN_ARM */", join_arm)
        .replace("/* MAIN_UNLOAD_FLUSH */", unload_flush))
    with tempfile.TemporaryDirectory(prefix="split_seam_") as scratch:
        work = Path(scratch)
        fixture = work / "split_seam.c"
        fixture.write_text(fixture_text, encoding="utf-8")
        exe = work / "split_seam.exe"
        subprocess.run([args.cc, "-std=c11", "-UNDEBUG", "-Wall",
                        str(fixture), "-o", str(exe)], check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == "__main__":
    main()

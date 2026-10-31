#include "mp_pause_menu_script.h"
#include <stdio.h>
#include <string.h>
#include "chat_login_script.h"
#include "mp_pause_menu_input.h"
#include "../world.h"
#include "../block/blocks.h"
#include "game_state.h"
#include "autopilot.h"
#include "mp_play_map.h"
#include "../platform/time.h"
#ifdef PLATFORM_WII
#include <ogc/lwp_watchdog.h>
#endif

static uint64_t marker_uptime_ms(void) {
    ptime_t now = time_get();
#ifdef PLATFORM_WII
    return (uint64_t)now / TB_TIMER_CLOCK;
#else
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

static const char* request_name(unsigned buttons) {
    if(buttons & MPM_FORWARD) return "forward";
    if(buttons & MPM_HOME) return "home";
    if(buttons & MPM_BACK) return "back";
    if(buttons & MPM_DOWN) return "down";
    if(buttons & MPM_CLICK) return "click";
    if(buttons & MPM_JUMP) return "jump";
    return "release";
}

/* The production world query behind the lane check (same query as the
 * column check below). */
static enum mpm_cell mpm_world_cell(void* user, int x, int y, int z) {
    uint8_t type=world_get_block(&gstate.world,x,y,z).type;
    (void)user;
    if(type==BLOCK_AIR) return MPM_CELL_AIR;
    return block_beta_material_is_solid(type) ? MPM_CELL_SOLID : MPM_CELL_OTHER;
}

static struct {
    bool armed, done_sent, pending, aim_held;
    struct mpm_state core;
    struct mp_pause_menu_hooks hooks;
    char username[32], line[448];
    unsigned seq;
    enum mpm_marker failure_marker;
} s;

/* The turn rides autopilot's aim seam (mode 1 forces no buttons, so the
 * driver's pad still walks); released on the first frame the core stops
 * asking, which hands the pre-seam autopilot state back. */
static void mpm_apply_aim(const struct mpm_out* out) {
    if(out->aim) {
        autopilot_script_aim(true,out->aim_yaw_deg,0);
        s.aim_held=true;
    } else if(s.aim_held) {
        autopilot_script_aim(false,0,0);
        s.aim_held=false;
    }
}

void mp_pause_menu_arm(const struct mp_pause_menu_hooks* hooks,
                       const char* username) {
    if(s.aim_held) autopilot_script_aim(false,0,0);
    memset(&s,0,sizeof(s));
    if(!hooks) return;
    s.hooks=*hooks;
    snprintf(s.username,sizeof(s.username),"%s",username ? username : "");
    s.armed=true;
}
bool mp_pause_menu_armed(void) { return s.armed; }

void mp_pause_menu_frame(const struct mpm_obs* obs) {
    struct mpm_obs o;
    struct mpm_out out;
    enum input_button buttons[6];
    unsigned n;
    bool pad_accepted=true;
    if(!s.armed || !obs || s.done_sent) return;
    if(s.pending) {
        if(!s.hooks.emit || !s.hooks.emit(s.hooks.user,s.line)) return;
        s.pending=false;
    }
    o=*obs;
    o.up_ms=marker_uptime_ms();
    if(o.world && o.body) {
        int x=mpp_floor(o.x), y=mpp_floor(o.y), z=mpp_floor(o.z);
        /* Same production-world query as mp_play_script.c:mpp_fill_target:
         * world_get_block(...).type, with feet Y already in block units. */
        o.site_clear=block_beta_material_is_solid(
                world_get_block(&gstate.world,x,y-1,z).type);
        for(int h=0;h<=3;h++)
            o.site_clear=o.site_clear &&
                world_get_block(&gstate.world,x,y+h,z).type==BLOCK_AIR;
        /* hw1022a: the site also needs a walkable lane on some heading (the
         * live facing or a cardinal) for the post-close walk. */
        o.lane_mask=mpm_lane_mask(mpm_world_cell,NULL,o.x,o.y,o.z,o.yaw_deg);
        /* The jump needs only a clear column. The lane is judged at pause
         * close (mpm_choose_heading), which never fails the run by itself. */
    }
    o.login_parked=chat_login_phase_now()==CHAT_LOGIN_PARKED;
    /* REL-14a DIAG section 5: a play-mode login failure sets FAILED without
     * latching play_fault; report it as itself, not as timeout:login. */
    o.login_failed=chat_login_phase_now()==CHAT_LOGIN_FAILED;
    mpm_tick(&s.core,&o,&out);
    if(chat_login_play_fault(NULL)) {
        out=(struct mpm_out){0};
        mpm_fail(&s.core,&out,MPM_MARK_FAIL,"login_failed",NULL);
    }
    if(out.start_login) {
        /* ACTION-ONLY login, as mp_play (REL-14b-0): vanilla Beta never
         * answers `/login` (CFR NetServerHandler.java:471-474), so the typed
         * login's p6 fixture ACK can never arrive on the R14 server. */
        chat_login_arm_play_actions(&(struct chat_login_hooks){
            .emit=s.hooks.emit, .chat_lines_sent=s.hooks.chat_lines_sent,
            .user=s.hooks.user},s.username);
    }
    n=mpm_map_buttons(out.buttons,buttons);
    if(o.login_parked) pad_accepted=chat_login_play_request(buttons,n);
    if(!pad_accepted) {
        /* A forward marker must never claim an input the production pad
         * refused. The only possible verdict is a named driver failure. */
        mpm_fail(&s.core,&out,MPM_MARK_FAIL,"pad_refused",
                 request_name(out.buttons));
        out.buttons=0;
        out.aim=false;
    }
    mpm_apply_aim(&out);
    if(out.marker!=MPM_MARK_NONE) {
        if(out.marker==MPM_MARK_SITE_UNVERIFIED || out.marker==MPM_MARK_APEX_SHORT)
            s.failure_marker=out.marker;
        if(!mp_pause_menu_emit_marker(s.line,sizeof(s.line),++s.seq,
                out.marker,&o,&s.core,out.buttons,out.reason,
                s.hooks.emit,s.hooks.user))
            s.pending=true;
    }
    if(s.core.phase==MPM_DONE || s.core.phase==MPM_FAIL) {
        if(s.pending) return;
        if(s.aim_held) { autopilot_script_aim(false,0,0); s.aim_held=false; }
        chat_login_play_end();
        if(s.hooks.profile_done)
            s.hooks.profile_done(s.hooks.user,
                    s.core.phase==MPM_DONE ? "ok" :
                    s.failure_marker==MPM_MARK_SITE_UNVERIFIED ? "site_unverified" :
                    s.failure_marker==MPM_MARK_APEX_SHORT ? "apex_short" :
                    "mp_pause_menu_failed");
        s.done_sent=true;
    }
}

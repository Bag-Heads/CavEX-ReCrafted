#ifndef GAME_MP_PAUSE_MENU_SCRIPT_H
#define GAME_MP_PAUSE_MENU_SCRIPT_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <math.h>
#include "mp_pause_menu_core.h"
struct mp_pause_menu_hooks {
    bool (*emit)(void*, const char*);
    void (*profile_done)(void*, const char*);
    bool (*chat_lines_sent)(void*, uint32_t*);
    void* user;
};
static inline const char* mp_pause_menu_marker_name(enum mpm_marker m) {
    switch(m) {
    case MPM_MARK_PAUSE_OPEN: return "pause_open";
    case MPM_MARK_HOLD_FORWARD: return "pause_hold_forward";
    case MPM_MARK_HOLD_END: return "pause_hold_end";
    case MPM_MARK_PAUSE_CLOSE: return "pause_close";
    case MPM_MARK_WALK_AFTER: return "walk_after";
    case MPM_MARK_DONE: return "done";
    case MPM_MARK_FAIL: return "fail";
    case MPM_MARK_SITE_UNVERIFIED: return "site_unverified";
    case MPM_MARK_APEX_SHORT: return "apex_short";
    default: return "unknown";
    }
}
/* Shared production/test seam: format the exact payload and pass it to emit. */
static inline bool mp_pause_menu_emit_marker(
        char* line, size_t capacity, unsigned seq, enum mpm_marker marker,
        const struct mpm_obs* o, const struct mpm_state* st,
        unsigned buttons, const char* reason,
        bool (*emit)(void*, const char*), void* user) {
    /* hw1022a lane record: the live facing, the walk heading (the turn's
     * heading once the driver turned, else the facing), the per-heading lane
     * mask (bit 0 facing, bit 1+k engine yaw k*90) and the verdict. */
    float yaw=fmodf(o->yaw_deg,360.0f);
    enum mpm_lane lane=st ? st->lane : MPM_LANE_UNCHECKED;
    int heading;
    if(yaw<0.0f) yaw+=360.0f;
    heading=lane==MPM_LANE_TURNED ? st->heading_deg : (int)(yaw+0.5f)%360;
    snprintf(line,capacity,
             "{\"event\":\"mp_pause_menu\",\"seq\":%u,"
             "\"marker\":\"%s\",\"ms\":%u,\"tick\":%u,\"up_ms\":%llu,"
             "\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,"
             "\"screen\":\"%s\",\"remote\":%s,"
             "\"forward\":%s,\"login\":%s,\"yaw\":%.1f,\"heading\":%d,"
             "\"lane\":\"%s\",\"lane_mask\":%u,\"reason\":\"%s\"}",
             seq,mp_pause_menu_marker_name(marker),o->ms,o->tick,
             (unsigned long long)o->up_ms,
             (double)o->x,(double)o->y,(double)o->z,
             o->pause ? "pause" : o->ingame ? "ingame" :
             o->title ? "title" : "other",o->remote ? "true" : "false",
             buttons & MPM_FORWARD ? "true" : "false",
             o->login_parked ? "true" : "false",(double)yaw,heading,
             mpm_lane_name(lane),
             lane==MPM_LANE_UNCHECKED ? o->lane_mask : st->lane_mask,
             reason ? reason : "");
    return emit && emit(user,line);
}
void mp_pause_menu_arm(const struct mp_pause_menu_hooks* hooks,
                       const char* username);
bool mp_pause_menu_armed(void);
void mp_pause_menu_frame(const struct mpm_obs* obs);
#endif

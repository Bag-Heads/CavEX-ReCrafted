/* Pure driver for the real-Wii multiplayer pause proof. */
#ifndef GAME_MP_PAUSE_MENU_CORE_H
#define GAME_MP_PAUSE_MENU_CORE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

enum mpm_phase { MPM_READY, MPM_LOGIN, MPM_SETTLE, MPM_SITE_RETRY, MPM_JUMP_PHASE,
    MPM_APEX, MPM_OPEN, MPM_HOLD, MPM_CLOSE, MPM_TURN, MPM_WALK,
    MPM_LEAVE_OPEN, MPM_LEAVE_SELECT, MPM_LEAVE_WAIT, MPM_DONE, MPM_FAIL };
enum mpm_marker { MPM_MARK_NONE, MPM_MARK_PAUSE_OPEN,
    MPM_MARK_HOLD_FORWARD, MPM_MARK_HOLD_END, MPM_MARK_PAUSE_CLOSE,
    MPM_MARK_WALK_AFTER, MPM_MARK_DONE, MPM_MARK_FAIL,
    MPM_MARK_SITE_UNVERIFIED, MPM_MARK_APEX_SHORT };
#define MPM_JUMP 1u
#define MPM_HOME 2u
#define MPM_FORWARD 4u
#define MPM_BACK 8u
#define MPM_DOWN 16u
#define MPM_CLICK 32u
/* REL-15: the open menu dwells 180 s, three times the vanilla server's
 * 1200-tick read timeout (CFR NetworkManager.java:214-215). Beta keeps a
 * multiplayer world ticking and sending behind any GUI (Minecraft.java:598,
 * :1150), so only a client that stops sending is dropped in that window. */
#define MPM_HOLD_MS 180000u
/* Whole-driver cap: login budget (65 s) + hold + jump/close/walk/leave.
 * driver_owns_completion means the device duration never cuts it short. */
#define MPM_TOTAL_MS 300000u

/* hw1022a: the post-close walk hit a stone wall at a random spawn and moved
 * 0.2 blocks, because the site check looked only at the player's own column.
 * The walking LANE is now checked too: for 3 blocks along a heading, air at
 * feet and head height and solid ground under each step. Bit 0 of lane_mask
 * is the lane along the body's live facing; bit 1+k is the lane along engine
 * yaw k*90 degrees (engine forward = (sin yaw, cos yaw): 0 = +Z, 90 = +X,
 * 180 = -Z, 270 = -X). */
#define MPM_LANE_STEPS 3
#define MPM_LANE_FACING_BIT 1u
#define MPM_LANE_CARDINAL_BIT(k) (2u << (k))
#define MPM_TURN_TOL_DEG 5.0f
#define MPM_TURN_MS 1500u
enum mpm_cell { MPM_CELL_OTHER, MPM_CELL_AIR, MPM_CELL_SOLID };
enum mpm_lane { MPM_LANE_UNCHECKED, MPM_LANE_FACING, MPM_LANE_TURNED,
    MPM_LANE_BLOCKED };
struct mpm_obs {
    uint32_t ms, tick;
    uint64_t up_ms;
    float x, y, z, vy;
    float yaw_deg; /* live body facing, engine convention, degrees */
    unsigned lane_mask;
    bool remote, world, body, ground, login_parked, login_failed;
    bool ingame, pause, title, leave_focus, site_clear;
};
/* aim: hold the camera at aim_yaw_deg (engine convention, level pitch). */
struct mpm_out { unsigned buttons; enum mpm_marker marker; bool start_login;
    bool aim; int aim_yaw_deg; char reason[64]; };
struct mpm_state {
    enum mpm_phase phase;
    uint32_t phase_ms, hold_ms, press_ms, deadline_ms;
    bool login_started, marked_open, marked_hold, marked_end;
    unsigned leave_steps;
    unsigned site_retries, retry_try, walk_try;
    float walk_x, walk_z;
    int walk_base_deg; /* cardinal nearest the first walk heading */
    bool walk_jump; /* blocked-heading retries hold JUMP to climb a 1-block step */
    float retry_x, retry_z, jump_y;
    int heading_deg; /* engine yaw of the pause-close turn; valid when lane == TURNED */
    enum mpm_lane lane;
    unsigned lane_mask; /* the lane mask the heading was chosen from */
};
static inline const char* mpm_lane_name(enum mpm_lane lane) {
    switch(lane) {
    case MPM_LANE_FACING: return "facing";
    case MPM_LANE_TURNED: return "turned";
    case MPM_LANE_BLOCKED: return "blocked";
    default: return "unchecked";
    }
}
static inline int mpm_floor_i(float v) {
    int i=(int)v;
    return (float)i > v ? i-1 : i;
}
/* One lane: MPM_LANE_STEPS cells along (fx,fz) from the feet position, each
 * with air at feet and head height and a solid block under it. `cell` is the
 * world query (production: world_get_block + block_beta_material_is_solid). */
static inline bool mpm_lane_clear(enum mpm_cell (*cell)(void*, int, int, int),
                                  void* user, float x, float y, float z,
                                  float fx, float fz) {
    int fy=mpm_floor_i(y);
    for(int d=1; d<=MPM_LANE_STEPS; d++) {
        int cx=mpm_floor_i(x+fx*(float)d), cz=mpm_floor_i(z+fz*(float)d);
        if(cell(user,cx,fy,cz)!=MPM_CELL_AIR ||
           cell(user,cx,fy+1,cz)!=MPM_CELL_AIR ||
           cell(user,cx,fy-1,cz)!=MPM_CELL_SOLID)
            return false;
    }
    return true;
}
static inline unsigned mpm_lane_mask(enum mpm_cell (*cell)(void*, int, int, int),
                                     void* user, float x, float y, float z,
                                     float yaw_deg) {
    static const float dx[4]={0.0f,1.0f,0.0f,-1.0f}, dz[4]={1.0f,0.0f,-1.0f,0.0f};
    float r=yaw_deg*3.14159265f/180.0f;
    unsigned mask=0;
    if(mpm_lane_clear(cell,user,x,y,z,sinf(r),cosf(r))) mask|=MPM_LANE_FACING_BIT;
    for(int k=0; k<4; k++)
        if(mpm_lane_clear(cell,user,x,y,z,dx[k],dz[k]))
            mask|=MPM_LANE_CARDINAL_BIT(k);
    return mask;
}
static inline float mpm_yaw_diff_deg(float a, float b) {
    float d=fmodf(a-b,360.0f);
    if(d<0.0f) d+=360.0f;
    return d>180.0f ? 360.0f-d : d;
}
/* Pick the post-close walk heading: keep the live facing when its lane is
 * clear, else the first clear cardinal (0, 90, 180, 270), else keep the
 * facing and record the lane as blocked (the walk still runs and the
 * collector's >=1 block rule judges it; the lane rule is stricter than
 * walkability, e.g. a step down, so it never fails the run by itself). */
static inline void mpm_choose_heading(struct mpm_state* s, unsigned mask) {
    s->lane_mask=mask; s->heading_deg=-1;
    if(mask & MPM_LANE_FACING_BIT) { s->lane=MPM_LANE_FACING; return; }
    for(int k=0; k<4; k++)
        if(mask & MPM_LANE_CARDINAL_BIT(k)) {
            s->lane=MPM_LANE_TURNED; s->heading_deg=k*90; return;
        }
    s->lane=MPM_LANE_BLOCKED;
}
static inline void mpm_enter(struct mpm_state* s, enum mpm_phase p, uint32_t ms) {
    s->phase = p; s->phase_ms = ms;
}
static inline const char* mpm_phase_name(enum mpm_phase phase) {
    switch(phase) {
    case MPM_READY: return "ready";
    case MPM_LOGIN: return "login";
    case MPM_SETTLE: return "settle";
    case MPM_SITE_RETRY: return "site_retry";
    case MPM_JUMP_PHASE: return "jump";
    case MPM_APEX: return "apex";
    case MPM_OPEN: return "open";
    case MPM_HOLD: return "hold";
    case MPM_CLOSE: return "close";
    case MPM_TURN: return "turn";
    case MPM_WALK: return "walk";
    case MPM_LEAVE_OPEN: return "leave_open";
    case MPM_LEAVE_SELECT: return "leave_select";
    case MPM_LEAVE_WAIT: return "leave_wait";
    default: return "unknown";
    }
}
static inline int mpm_cardinal_near(float yaw_deg) {
    float d=fmodf(yaw_deg,360.0f);
    int k;
    if(d<0.0f) d+=360.0f;
    k=(int)((d+45.0f)/90.0f)%4;
    return k*90;
}
/* Start (or restart) the post-close walk: remember where it began so the
 * walk can be judged by distance moved, not by elapsed time. */
static inline void mpm_begin_walk(struct mpm_state* s, const struct mpm_obs* o) {
    s->walk_x=o->x; s->walk_z=o->z;
    mpm_enter(s, MPM_WALK, o->ms);
}
static inline void mpm_fail(struct mpm_state* s, struct mpm_out* out,
                            enum mpm_marker marker, const char* cause,
                            const char* detail) {
    s->phase=MPM_FAIL; out->marker=marker; out->aim=false;
    if(detail) snprintf(out->reason,sizeof(out->reason),"%s:%s",cause,detail);
    else snprintf(out->reason,sizeof(out->reason),"%s",cause);
}
static inline void mpm_tick(struct mpm_state* s, const struct mpm_obs* o,
                     struct mpm_out* out) {
    *out = (struct mpm_out){0};
    if(s->phase == MPM_DONE || s->phase == MPM_FAIL) return;
    /* The cap starts at the in-game moment (READY -> LOGIN), not at boot or
     * connect, so a slow join cannot eat the driver's own budget. */
    if(s->deadline_ms && o->ms >= s->deadline_ms) {
        mpm_fail(s,out,MPM_MARK_FAIL,"timeout",mpm_phase_name(s->phase)); return;
    }
    if(s->phase >= MPM_SETTLE && s->phase <= MPM_WALK && !o->remote) {
        mpm_fail(s,out,MPM_MARK_FAIL,"session_lost",mpm_phase_name(s->phase)); return;
    }
    switch(s->phase) {
    case MPM_READY:
        if(o->remote && o->world && o->body && o->ingame) {
            out->start_login = true; s->login_started = true;
            s->deadline_ms = o->ms + MPM_TOTAL_MS;
            mpm_enter(s, MPM_LOGIN, o->ms);
        }
        break;
    case MPM_LOGIN:
        if(o->login_failed) mpm_fail(s,out,MPM_MARK_FAIL,"login_failed",NULL);
        else if(o->login_parked) mpm_enter(s, MPM_SETTLE, o->ms);
        else if(o->ms - s->phase_ms > 65000u) {
            mpm_fail(s,out,MPM_MARK_FAIL,"timeout",mpm_phase_name(s->phase));
        }
        break;
    case MPM_SETTLE:
        if(o->ingame && o->ground && o->ms - s->phase_ms >= 500u) {
            if(o->site_clear) {
                s->jump_y=o->y;
                mpm_enter(s, MPM_JUMP_PHASE, o->ms);
            } else if(s->site_retries >= 3u) {
                /* Record the lane now so the marker never says "unchecked". */
                mpm_choose_heading(s, o->lane_mask);
                mpm_fail(s,out,MPM_MARK_SITE_UNVERIFIED,"site_unverified",NULL);
            } else {
                s->retry_try=0u;
                s->retry_x=o->x; s->retry_z=o->z;
                mpm_enter(s, MPM_SITE_RETRY, o->ms);
            }
        }
        break;
    case MPM_SITE_RETRY: {
        /* Try the live facing first, then each cardinal (0/90/180/270), each
         * for up to 3 s with FORWARD+JUMP (a 1-block step is climbable in
         * normal Beta input). Only after every heading failed to move the
         * body 1 block is the site unverified. */
        float dx=o->x-s->retry_x, dz=o->z-s->retry_z;
        if(dx*dx+dz*dz >= 1.0f) {
            s->site_retries++;
            s->retry_try=0u;
            mpm_enter(s, MPM_SETTLE, o->ms-500u);
        } else if(o->ms-s->phase_ms > 3000u) {
            if(++s->retry_try > 4u) {
                mpm_choose_heading(s, o->lane_mask);
                mpm_fail(s,out,MPM_MARK_SITE_UNVERIFIED,"site_unverified",NULL);
            } else {
                s->retry_x=o->x; s->retry_z=o->z;
                mpm_enter(s, MPM_SITE_RETRY, o->ms);
            }
        } else if(s->retry_try==0u) {
            out->buttons=MPM_FORWARD|MPM_JUMP;
        } else {
            int h=(int)(s->retry_try-1u)*90;
            out->aim=true; out->aim_yaw_deg=h;
            if(mpm_yaw_diff_deg(o->yaw_deg,(float)h) <= MPM_TURN_TOL_DEG)
                out->buttons=MPM_FORWARD|MPM_JUMP;
        }
        break;
    }
    case MPM_JUMP_PHASE:
        out->buttons = MPM_JUMP;
        if(!o->ground && o->vy > 0.0f) mpm_enter(s, MPM_APEX, o->ms);
        else if(o->ms - s->phase_ms > 1500u) {
            mpm_fail(s,out,MPM_MARK_FAIL,"timeout",mpm_phase_name(s->phase));
        }
        break;
    case MPM_APEX:
        if(!o->ground && o->vy <= 0.0f) {
            if(o->y-s->jump_y < 0.9f) {
                mpm_fail(s,out,MPM_MARK_APEX_SHORT,"apex_short",NULL);
                break;
            }
            out->buttons = MPM_HOME; s->press_ms = o->ms;
            mpm_enter(s, MPM_OPEN, o->ms);
        } else if(o->ms - s->phase_ms > 1500u) {
            mpm_fail(s,out,MPM_MARK_FAIL,"timeout",mpm_phase_name(s->phase));
        }
        break;
    case MPM_OPEN:
        if(o->pause) {
            out->marker = MPM_MARK_PAUSE_OPEN;
            out->buttons = MPM_FORWARD;
            mpm_enter(s, MPM_HOLD, o->ms);
        } else if(o->ms - s->phase_ms > 1200u) {
            mpm_fail(s,out,MPM_MARK_FAIL,"timeout",mpm_phase_name(s->phase));
        }
        break;
    case MPM_HOLD:
        if(!o->pause) { mpm_fail(s,out,MPM_MARK_FAIL,"session_lost",mpm_phase_name(s->phase)); break; }
        out->buttons = MPM_FORWARD;
        if(!s->marked_hold) {
            s->marked_hold = true; s->hold_ms = o->ms;
            out->marker = MPM_MARK_HOLD_FORWARD;
        } else if(o->ms - s->hold_ms >= MPM_HOLD_MS) {
            out->marker = MPM_MARK_HOLD_END;
            mpm_enter(s, MPM_CLOSE, o->ms);
        }
        break;
    case MPM_CLOSE:
        if(o->pause) { out->buttons = MPM_BACK; break; }
        if(o->ingame) {
            out->marker = MPM_MARK_PAUSE_CLOSE;
            mpm_choose_heading(s, o->lane_mask);
            if(s->lane == MPM_LANE_TURNED) {
                /* Turn first, without walking, then walk the clear lane. */
                out->aim = true; out->aim_yaw_deg = s->heading_deg;
                s->walk_base_deg = s->heading_deg;
                mpm_enter(s, MPM_TURN, o->ms);
            } else {
                out->buttons = MPM_FORWARD;
                s->walk_base_deg = mpm_cardinal_near(o->yaw_deg);
                mpm_begin_walk(s, o);
            }
        }
        break;
    case MPM_TURN:
        if(!o->ingame) { mpm_fail(s,out,MPM_MARK_FAIL,"session_lost",mpm_phase_name(s->phase)); break; }
        out->aim = true; out->aim_yaw_deg = s->heading_deg;
        if(mpm_yaw_diff_deg(o->yaw_deg,(float)s->heading_deg) <= MPM_TURN_TOL_DEG) {
            out->buttons = MPM_FORWARD | (s->walk_jump ? MPM_JUMP : 0u);
            mpm_begin_walk(s, o);
        } else if(o->ms - s->phase_ms > MPM_TURN_MS) {
            mpm_fail(s,out,MPM_MARK_FAIL,"timeout",mpm_phase_name(s->phase));
        }
        break;
    case MPM_WALK:
        if(!o->ingame) { mpm_fail(s,out,MPM_MARK_FAIL,"session_lost",mpm_phase_name(s->phase)); break; }
        out->buttons = MPM_FORWARD | (s->walk_jump ? MPM_JUMP : 0u);
        if(s->lane == MPM_LANE_TURNED) { out->aim = true; out->aim_yaw_deg = s->heading_deg; }
        if(o->ms - s->phase_ms >= 2000u) {
            float dx=o->x-s->walk_x, dz=o->z-s->walk_z;
            if(dx*dx+dz*dz >= 1.0f) {
                /* No-false-PASS: walk_after is emitted only after the body
                 * really moved 1 block or more. */
                out->buttons = 0u; out->aim = false;
                out->marker = MPM_MARK_WALK_AFTER;
                mpm_enter(s, MPM_LEAVE_OPEN, o->ms);
            } else if(s->walk_try < 3u) {
                /* Blocked heading: turn to the next cardinal and walk it
                 * with a jump. */
                s->walk_try++;
                s->walk_jump=true;
                s->lane=MPM_LANE_TURNED;
                s->heading_deg=(s->walk_base_deg+90*(int)s->walk_try)%360;
                out->aim = true; out->aim_yaw_deg = s->heading_deg;
                out->buttons = 0u;
                mpm_enter(s, MPM_TURN, o->ms);
            } else {
                mpm_fail(s,out,MPM_MARK_FAIL,"walk_blocked",NULL);
            }
        }
        break;
    case MPM_LEAVE_OPEN:
        if(o->pause) { mpm_enter(s, MPM_LEAVE_SELECT, o->ms); break; }
        out->buttons = MPM_HOME;
        break;
    case MPM_LEAVE_SELECT:
        if(!o->pause) { mpm_fail(s,out,MPM_MARK_FAIL,"session_lost",mpm_phase_name(s->phase)); break; }
        if(o->leave_focus) {
            if(o->ms - s->press_ms >= 220u) {
                out->buttons = MPM_CLICK;
                mpm_enter(s, MPM_LEAVE_WAIT, o->ms);
            }
        } else if(o->ms - s->press_ms >= 220u) {
            out->buttons = MPM_DOWN; s->press_ms = o->ms;
            if(++s->leave_steps > 10u) {
                mpm_fail(s,out,MPM_MARK_FAIL,"leave_focus_unreachable",NULL);
            }
        }
        break;
    case MPM_LEAVE_WAIT:
        if(o->title && !o->remote) {
            s->phase = MPM_DONE; out->marker = MPM_MARK_DONE;
        }
        break;
    default: break;
    }
}
#endif

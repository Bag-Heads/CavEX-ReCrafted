#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "game/mp_pause_menu_core.h"
#include "game/mp_pause_menu_input.h"
#include "game/mp_pause_menu_script.h"

static bool capture_marker(void* user, const char* line) {
    assert(user==line);
    return true;
}


/* hw1022a lane fixture: a 16x8x16 cell box at origin (-8,60,-8). Feet at
 * y=64 on a stone floor (y=63), everything else air unless set. */
static enum mpm_cell lane_world[16][8][16];
static void lane_reset(void) {
    for(int x=0;x<16;x++) for(int y=0;y<8;y++) for(int z=0;z<16;z++)
        lane_world[x][y][z]= y==3 ? MPM_CELL_SOLID : MPM_CELL_AIR;
}
static void lane_set(int x, int y, int z, enum mpm_cell c) {
    lane_world[x+8][y-60][z+8]=c;
}
static enum mpm_cell lane_cell(void* user, int x, int y, int z) {
    (void)user;
    x+=8; y-=60; z+=8;
    if(x<0||x>=16||y<0||y>=8||z<0||z>=16) return MPM_CELL_OTHER;
    return lane_world[x][y][z];
}
int main(void) {
    struct mpm_state s = {0};
    struct mpm_obs o = {.remote=true,.world=true,.body=true,.ingame=true,
        .ground=true, .site_clear=true};
    struct mpm_out out;
    enum mpm_marker order[6] = {0};
    unsigned count = 0;
    bool hold = false, home = false;
    enum input_button codes[6];
    char marker_line[448];
    assert(mpm_map_buttons(MPM_HOME, codes)==1 && codes[0]==IB_HOME);
    assert(mpm_map_buttons(MPM_FORWARD, codes)==1 && codes[0]==IB_FORWARD);
    assert(mpm_map_buttons(MPM_BACK, codes)==1 && codes[0]==IB_INVENTORY);
    for(o.ms=1; o.ms<400000 && s.phase!=MPM_DONE; o.ms+=50, o.tick++) {
        o.up_ms=100000u+o.ms;
        if(s.phase==MPM_LOGIN) o.login_parked=true;
        if(s.phase==MPM_JUMP_PHASE && o.ms-s.phase_ms>=100) {
            o.ground=false; o.vy=0.2f; o.y+=1.0f;
        }
        if(s.phase==MPM_APEX) o.vy=-0.01f;
        if(s.phase==MPM_OPEN && home) { o.pause=true; o.ingame=false; }
        if(s.phase==MPM_CLOSE) { o.pause=false; o.ingame=true; }
        if(s.phase==MPM_WALK) o.x+=0.05f; /* the body really walks */
        if(s.phase==MPM_LEAVE_OPEN) { o.pause=true; o.ingame=false; }
        if(s.phase==MPM_LEAVE_SELECT) o.leave_focus=true;
        if(s.phase==MPM_LEAVE_WAIT) { o.title=true; o.remote=false; }
        mpm_tick(&s,&o,&out);
        if(out.buttons & MPM_HOME) {
            assert(mpm_map_buttons(out.buttons,codes)>=1 && codes[0]==IB_HOME);
            home=true;
        }
        if(s.phase==MPM_HOLD || s.phase==MPM_CLOSE)
            assert((out.buttons & MPM_FORWARD) || s.phase==MPM_CLOSE);
        if(out.marker==MPM_MARK_HOLD_FORWARD) hold=true;
        if(out.marker==MPM_MARK_HOLD_END) {
            assert(hold && (out.buttons & MPM_FORWARD));
            /* REL-15: the open menu dwells 180 s, three times the vanilla
             * server's 1200-tick read timeout (CFR NetworkManager.java:214-215),
             * so a client that stops sending while paused would be kicked. */
            assert(o.ms-s.hold_ms>=180000u);
        }
        if(out.marker && count<6) {
            assert(o.up_ms==100000u+o.ms);
            if(out.marker==MPM_MARK_PAUSE_OPEN ||
               out.marker==MPM_MARK_PAUSE_CLOSE ||
               out.marker==MPM_MARK_WALK_AFTER) {
                assert(mp_pause_menu_emit_marker(marker_line, sizeof(marker_line),
                       count+1, out.marker, &o, &s, out.buttons, out.reason,
                       capture_marker, marker_line));
                assert(strstr(marker_line,"\"up_ms\":") &&
                       strstr(marker_line,"\"tick\":") &&
                       strstr(marker_line,"\"x\":") &&
                       strstr(marker_line,"\"y\":") &&
                       strstr(marker_line,"\"z\":"));
                if(out.marker==MPM_MARK_PAUSE_OPEN) {
                    char expected[48];
                    snprintf(expected,sizeof(expected),"\"up_ms\":%llu",
                             (unsigned long long)o.up_ms);
                    assert(strstr(marker_line,expected));
                    snprintf(expected,sizeof(expected),"\"tick\":%u",o.tick);
                    assert(strstr(marker_line,expected));
                    snprintf(expected,sizeof(expected),"\"x\":%.4f",(double)o.x);
                    assert(strstr(marker_line,expected));
                    snprintf(expected,sizeof(expected),"\"y\":%.4f",(double)o.y);
                    assert(strstr(marker_line,expected));
                    snprintf(expected,sizeof(expected),"\"z\":%.4f",(double)o.z);
                    assert(strstr(marker_line,expected));
                    assert(strstr(marker_line,"\"marker\":\"pause_open\""));
                    assert(strstr(marker_line,"\"screen\":\"pause\""));
                }
            }
            order[count++]=out.marker;
        }
    }
    assert(s.phase==MPM_DONE && count==6);
    assert(order[0]==MPM_MARK_PAUSE_OPEN);
    assert(order[1]==MPM_MARK_HOLD_FORWARD);
    assert(order[2]==MPM_MARK_HOLD_END);
    assert(order[3]==MPM_MARK_PAUSE_CLOSE);
    assert(order[4]==MPM_MARK_WALK_AFTER);
    assert(order[5]==MPM_MARK_DONE);
    puts("PASS mp_pause_menu_core: HOME, sustained forward, ordered markers");
    {
        struct mpm_state blocked={0};
        struct mpm_obs site={.remote=true,.world=true,.body=true,
            .ingame=true,.ground=true,.site_clear=false};
        unsigned moves=0;
        for(site.ms=1;site.ms<10000 && blocked.phase!=MPM_FAIL;site.ms+=50) {
            site.login_parked=true;
            mpm_tick(&blocked,&site,&out);
            if(out.buttons & MPM_FORWARD) { moves++; site.x+=0.1f; }
        }
        assert(moves>=30 && blocked.site_retries==3 && blocked.phase==MPM_FAIL);
        assert(out.marker==MPM_MARK_SITE_UNVERIFIED);
    }
    {
        struct mpm_state short_jump={.phase=MPM_APEX,.jump_y=65.0f,
            .deadline_ms=10000};
        struct mpm_obs short_obs={.ms=100,.remote=true,.ingame=true,
            .ground=false,.vy=-0.01f,.y=65.5f};
        mpm_tick(&short_jump,&short_obs,&out);
        assert(short_jump.phase==MPM_FAIL && out.marker==MPM_MARK_APEX_SHORT);
    }
    puts("PASS mp_pause_menu_core: site_unverified, apex_short");
    {
        struct {
            struct mpm_state state;
            struct mpm_obs obs;
            const char* reason;
        } failures[] = {
            {{.phase=MPM_HOLD,.deadline_ms=100}, {.ms=100,.remote=true}, "timeout:hold"},
            {{.phase=MPM_HOLD,.deadline_ms=1000}, {.ms=100,.remote=false}, "session_lost:hold"},
            {{.phase=MPM_HOLD,.deadline_ms=1000}, {.ms=100,.remote=true,.pause=false}, "session_lost:hold"},
            {{.phase=MPM_SETTLE,.deadline_ms=1000,.site_retries=3},
             {.ms=600,.remote=true,.ingame=true,.ground=true,.site_clear=false}, "site_unverified"},
            {{.phase=MPM_APEX,.deadline_ms=1000,.jump_y=65.0f},
             {.ms=100,.remote=true,.ground=false,.vy=-0.1f,.y=65.5f}, "apex_short"},
            /* REL-15 diag (REL-14a DIAG.md section 5): a failed login must
             * name itself at once, never wait out the 65 s budget as
             * timeout:login. */
            {{.phase=MPM_LOGIN,.deadline_ms=100000}, {.ms=100,.remote=true,.login_failed=true}, "login_failed"},
            {{.phase=MPM_LEAVE_SELECT,.deadline_ms=1000,.leave_steps=10},
             {.ms=300,.remote=true,.pause=true,.leave_focus=false}, "leave_focus_unreachable"},
        };
        for(size_t i=0;i<sizeof(failures)/sizeof(failures[0]);i++) {
            mpm_tick(&failures[i].state,&failures[i].obs,&out);
            assert(failures[i].state.phase==MPM_FAIL);
            assert(strcmp(out.reason,failures[i].reason)==0);
            assert(mp_pause_menu_emit_marker(marker_line,sizeof(marker_line),
                   1,out.marker,&failures[i].obs,&failures[i].state,0,out.reason,
                   capture_marker,marker_line));
            char expected[100];
            snprintf(expected,sizeof(expected),"\"reason\":\"%s\"",failures[i].reason);
            assert(strstr(marker_line,expected));
        }
        for(size_t i=0;i<2;i++) {
            const char* reason=i ? "pad_refused:forward" : "login_failed";
            assert(mp_pause_menu_emit_marker(marker_line,sizeof(marker_line),
                   1,MPM_MARK_FAIL,&o,NULL,0,reason,capture_marker,marker_line));
            char expected[100];
            snprintf(expected,sizeof(expected),"\"reason\":\"%s\"",reason);
            assert(strstr(marker_line,expected));
        }
    }
    puts("PASS mp_pause_menu_core: failure causes in marker payload");
    {
        /* hw1022a lane check: air at feet and head height for 3 blocks plus
         * solid ground under each step, per heading. */
        unsigned mask;
        lane_reset();
        mask=mpm_lane_mask(lane_cell,NULL,0.5f,64.0f,0.5f,0.0f);
        assert(mask==(MPM_LANE_FACING_BIT|MPM_LANE_CARDINAL_BIT(0)|
                      MPM_LANE_CARDINAL_BIT(1)|MPM_LANE_CARDINAL_BIT(2)|
                      MPM_LANE_CARDINAL_BIT(3)));
        /* The hw1022a case: a stone wall 2 blocks ahead (+Z, yaw 0). */
        for(int x=-8;x<8;x++) lane_set(x,64,2,MPM_CELL_SOLID);
        mask=mpm_lane_mask(lane_cell,NULL,0.5f,64.0f,0.5f,0.0f);
        assert(!(mask & MPM_LANE_FACING_BIT));
        assert(!(mask & MPM_LANE_CARDINAL_BIT(0)));
        assert(mask & MPM_LANE_CARDINAL_BIT(1));
        /* A head-height block on +X, a hole in the floor on -Z. */
        lane_set(3,65,0,MPM_CELL_SOLID);
        lane_set(0,63,-2,MPM_CELL_AIR);
        /* A non-solid (e.g. water) step under -X counts as not ground. */
        lane_set(-1,63,0,MPM_CELL_OTHER);
        mask=mpm_lane_mask(lane_cell,NULL,0.5f,64.0f,0.5f,0.0f);
        assert(mask==0);
        lane_set(-1,63,0,MPM_CELL_SOLID);
        mask=mpm_lane_mask(lane_cell,NULL,0.5f,64.0f,0.5f,0.0f);
        assert(mask==MPM_LANE_CARDINAL_BIT(3));
        /* Facing 270 (-X) uses the live facing lane. */
        mask=mpm_lane_mask(lane_cell,NULL,0.5f,64.0f,0.5f,270.0f);
        assert(mask==(MPM_LANE_FACING_BIT|MPM_LANE_CARDINAL_BIT(3)));
    }
    puts("PASS mp_pause_menu_core: walking lane check (feet, head, ground)");
    {
        /* Blocked facing: turn to the first clear cardinal (90 before 180),
         * walk only once the facing arrived, keep the aim through the walk,
         * release it after walk_after, and record heading + lane. */
        struct mpm_state t={.phase=MPM_CLOSE,.deadline_ms=100000};
        struct mpm_obs w={.ms=100,.remote=true,.ingame=true,.ground=true,
            .yaw_deg=0.0f,
            .lane_mask=MPM_LANE_CARDINAL_BIT(1)|MPM_LANE_CARDINAL_BIT(2)};
        uint32_t walk_start;
        mpm_tick(&t,&w,&out);
        assert(out.marker==MPM_MARK_PAUSE_CLOSE && t.phase==MPM_TURN);
        assert(t.lane==MPM_LANE_TURNED && t.heading_deg==90);
        assert(out.aim && out.aim_yaw_deg==90 && !(out.buttons & MPM_FORWARD));
        assert(mp_pause_menu_emit_marker(marker_line,sizeof(marker_line),4,
               out.marker,&w,&t,out.buttons,out.reason,capture_marker,
               marker_line));
        assert(strstr(marker_line,"\"lane\":\"turned\""));
        assert(strstr(marker_line,"\"heading\":90,"));
        assert(strstr(marker_line,"\"lane_mask\":12,"));
        assert(strstr(marker_line,"\"yaw\":0.0,"));
        w.ms=150;
        mpm_tick(&t,&w,&out);
        assert(t.phase==MPM_TURN && out.aim && !(out.buttons & MPM_FORWARD));
        w.ms=200; w.yaw_deg=-269.0f; /* = 91 deg, within tolerance */
        mpm_tick(&t,&w,&out);
        assert(t.phase==MPM_WALK && out.aim && out.aim_yaw_deg==90 &&
               (out.buttons & MPM_FORWARD));
        walk_start=w.ms;
        for(w.ms=250; t.phase==MPM_WALK; w.ms+=50) {
            w.x+=0.05f;
            mpm_tick(&t,&w,&out);
            if(t.phase==MPM_WALK) assert(out.aim && (out.buttons & MPM_FORWARD));
        }
        assert(out.marker==MPM_MARK_WALK_AFTER && w.ms-50-walk_start>=2000u);
        assert(mp_pause_menu_emit_marker(marker_line,sizeof(marker_line),5,
               out.marker,&w,&t,out.buttons,out.reason,capture_marker,
               marker_line));
        assert(strstr(marker_line,"\"heading\":90,") &&
               strstr(marker_line,"\"yaw\":91.0,"));
        mpm_tick(&t,&w,&out);
        assert(t.phase==MPM_LEAVE_OPEN && !out.aim);
    }
    {
        /* Clear facing: no turn, walk at once, lane recorded as facing. */
        struct mpm_state t={.phase=MPM_CLOSE,.deadline_ms=100000};
        struct mpm_obs w={.ms=100,.remote=true,.ingame=true,.yaw_deg=37.0f,
            .lane_mask=MPM_LANE_FACING_BIT|MPM_LANE_CARDINAL_BIT(1)};
        mpm_tick(&t,&w,&out);
        assert(t.phase==MPM_WALK && t.lane==MPM_LANE_FACING && !out.aim &&
               (out.buttons & MPM_FORWARD));
        assert(mp_pause_menu_emit_marker(marker_line,sizeof(marker_line),4,
               out.marker,&w,&t,out.buttons,out.reason,capture_marker,
               marker_line));
        assert(strstr(marker_line,"\"lane\":\"facing\"") &&
               strstr(marker_line,"\"heading\":37,"));
        w.ms=150; mpm_tick(&t,&w,&out);
        assert(!out.aim && (out.buttons & MPM_FORWARD));
    }
    {
        /* No clear lane: walk on the facing (the collector judges it),
         * recorded as blocked. */
        struct mpm_state t={.phase=MPM_CLOSE,.deadline_ms=100000};
        struct mpm_obs w={.ms=100,.remote=true,.ingame=true,.lane_mask=0};
        mpm_tick(&t,&w,&out);
        assert(t.phase==MPM_WALK && t.lane==MPM_LANE_BLOCKED && !out.aim);
    }
    {
        /* A turn that never arrives fails by name. */
        struct mpm_state t={.phase=MPM_CLOSE,.deadline_ms=100000};
        struct mpm_obs w={.ms=100,.remote=true,.ingame=true,.yaw_deg=0.0f,
            .lane_mask=MPM_LANE_CARDINAL_BIT(2)};
        mpm_tick(&t,&w,&out);
        assert(t.phase==MPM_TURN && out.aim_yaw_deg==180);
        for(w.ms=150; t.phase==MPM_TURN; w.ms+=50) mpm_tick(&t,&w,&out);
        assert(t.phase==MPM_FAIL && strcmp(out.reason,"timeout:turn")==0);
        assert(!out.aim); /* a failed turn hands the camera back */
    }
    puts("PASS mp_pause_menu_core: blocked lane turns to first clear cardinal");
    {
        /* Blocked walk: facing 0 goes nowhere. Try 90, 180, 270 with a jump;
         * the body moves only on 180. walk_after comes only after >=1 block. */
        struct mpm_state t={.phase=MPM_CLOSE,.deadline_ms=1000000};
        struct mpm_obs w={.ms=100,.remote=true,.ingame=true,.yaw_deg=0.0f,
            .lane_mask=0};
        int seen[3]={-1,-1,-1}, n=0;
        bool marked=false, jumped=false;
        mpm_tick(&t,&w,&out);
        assert(t.phase==MPM_WALK && t.lane==MPM_LANE_BLOCKED);
        for(w.ms=150; w.ms<60000 && t.phase!=MPM_LEAVE_OPEN &&
                      t.phase!=MPM_FAIL; w.ms+=50) {
            mpm_tick(&t,&w,&out);
            if(out.aim) w.yaw_deg=(float)out.aim_yaw_deg; /* camera obeys */
            if(t.phase==MPM_TURN && (n==0 || seen[n-1]!=t.heading_deg) && n<3)
                seen[n++]=t.heading_deg;
            if(t.phase==MPM_WALK && t.walk_try>0u) {
                assert(out.buttons & MPM_JUMP);
                jumped=true;
                if(t.heading_deg==180) w.z-=0.05f;
            }
            if(out.marker==MPM_MARK_WALK_AFTER) marked=true;
        }
        assert(n==2 && seen[0]==90 && seen[1]==180 && jumped && marked);
        assert(t.phase==MPM_LEAVE_OPEN && t.walk_try==2u);
    }
    {
        /* Every heading blocked: no walk_after, a named failure. */
        struct mpm_state t={.phase=MPM_CLOSE,.deadline_ms=1000000};
        struct mpm_obs w={.ms=100,.remote=true,.ingame=true,.yaw_deg=0.0f};
        bool marked=false;
        for(; w.ms<60000 && t.phase!=MPM_FAIL; w.ms+=50) {
            mpm_tick(&t,&w,&out);
            if(out.aim) w.yaw_deg=(float)out.aim_yaw_deg;
            if(out.marker==MPM_MARK_WALK_AFTER) marked=true;
        }
        assert(!marked && t.phase==MPM_FAIL && t.walk_try==3u &&
               strcmp(out.reason,"walk_blocked")==0);
    }
    {
        /* Site retry: a blocked facing then a clear cardinal. Every heading
         * is tried with FORWARD+JUMP; only 90 moves. The lane is recorded. */
        struct mpm_state t={.phase=MPM_SETTLE,.deadline_ms=1000000};
        struct mpm_obs w={.ms=1000,.remote=true,.world=true,.body=true,
            .ingame=true,.ground=true,.site_clear=false,.yaw_deg=0.0f};
        bool saw90=false;
        for(; w.ms<60000 && t.phase!=MPM_JUMP_PHASE && t.phase!=MPM_FAIL; w.ms+=50) {
            mpm_tick(&t,&w,&out);
            if(out.aim) w.yaw_deg=(float)out.aim_yaw_deg;
            if(t.phase==MPM_SITE_RETRY && out.aim && out.aim_yaw_deg==90 &&
               (out.buttons & MPM_JUMP)) { saw90=true; w.x+=0.1f; w.site_clear=true; }
            if(t.phase==MPM_SITE_RETRY && t.retry_try==0u)
                assert(!(out.buttons&MPM_FORWARD) || (out.buttons&MPM_JUMP));
        }
        assert(saw90 && t.phase==MPM_JUMP_PHASE);
        /* All blocked: unverified only after facing + 4 cardinals, lane named. */
        t=(struct mpm_state){.phase=MPM_SETTLE,.deadline_ms=1000000};
        w.site_clear=false; w.x=0.0f; w.lane_mask=0; w.ms=1000;
        for(; w.ms<60000 && t.phase!=MPM_FAIL; w.ms+=50) {
            mpm_tick(&t,&w,&out);
            if(out.aim) w.yaw_deg=(float)out.aim_yaw_deg;
        }
        assert(t.phase==MPM_FAIL && out.marker==MPM_MARK_SITE_UNVERIFIED &&
               w.ms>=1000u+5u*3000u);
        assert(mp_pause_menu_emit_marker(marker_line,sizeof(marker_line),9,
               out.marker,&w,&t,out.buttons,out.reason,capture_marker,
               marker_line));
        assert(strstr(marker_line,"\"lane\":\"blocked\""));
    }
    {
        /* The whole-driver cap starts at READY -> LOGIN, not at the first tick. */
        struct mpm_state t={0};
        struct mpm_obs w={.ms=200000,.remote=true,.world=true,.body=true};
        mpm_tick(&t,&w,&out);          /* not in game: waiting, no cap armed */
        assert(t.phase==MPM_READY && t.deadline_ms==0u);
        w.ms=500000; w.ingame=true; mpm_tick(&t,&w,&out);
        assert(t.phase==MPM_LOGIN && t.deadline_ms==500000u+MPM_TOTAL_MS);
    }
    puts("PASS mp_pause_menu_core: blocked-heading walk and site retry, deadline from in-game");
    return 0;
}

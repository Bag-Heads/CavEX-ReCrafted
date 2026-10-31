/* The only adapter from pure pause-driver actions to production input codes. */
#ifndef GAME_MP_PAUSE_MENU_INPUT_H
#define GAME_MP_PAUSE_MENU_INPUT_H
#include "mp_pause_menu_core.h"
#include "../platform/input.h"
static inline unsigned mpm_map_buttons(unsigned bits, enum input_button out[6]) {
    unsigned n=0;
    if(bits & MPM_JUMP) out[n++]=IB_JUMP;
    if(bits & MPM_HOME) out[n++]=IB_HOME;
    if(bits & MPM_FORWARD) out[n++]=IB_FORWARD;
    if(bits & MPM_BACK) out[n++]=IB_INVENTORY;
    if(bits & MPM_DOWN) out[n++]=IB_GUI_DOWN;
    if(bits & MPM_CLICK) out[n++]=IB_GUI_CLICK;
    return n;
}
#endif

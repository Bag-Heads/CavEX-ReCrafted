#ifndef CW_REMOTE_CAPTURE_H
#define CW_REMOTE_CAPTURE_H
#include <stdbool.h>
/* Opt-in authenticated, one-shot capture. All GX work remains on main. */
void remote_capture_init(bool enabled, const char* build);
void remote_capture_frame(void);
void remote_capture_shutdown(void);
#endif

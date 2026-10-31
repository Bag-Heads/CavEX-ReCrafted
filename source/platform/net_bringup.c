#ifdef PLATFORM_WII
#include "wii/net_bringup.c"
#else
/* PC needs no bring-up (WSAStartup is cheap + synchronous); this keeps the
 * translation unit non-empty for -pedantic. */
typedef int cw_net_bringup_pc_has_no_backend;
#endif

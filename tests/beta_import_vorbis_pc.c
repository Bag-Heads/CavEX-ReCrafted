/* PC stand-in for the engine's one stb_vorbis implementation TU
 * (audio_registry.c, which the beta_import test links leave out). Same
 * single-TU configuration as audio_registry.c and tools/sound_transcode.c,
 * so beta_import_sound.c's header-only declarations resolve here. Never
 * built for the Wii (tests/ is not in the Makefile's SOURCES). */
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 2
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-value"
#include <stb/stb_vorbis.c>
#pragma GCC diagnostic pop

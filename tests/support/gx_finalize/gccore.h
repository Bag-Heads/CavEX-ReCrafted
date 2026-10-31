/* Host-only GX sink: records the actual production CALL and flush ranges. */
#ifndef TEST_GX_FINALIZE_H
#define TEST_GX_FINALIZE_H
#include <stddef.h>
#include <stdint.h>
#define GX_QUADS 0x80
#define GX_VTXFMT0 0
#define GX_VTXFMT4 4
static void* gx_call_ptr;
static size_t gx_call_bytes, gx_flush_bytes;
static void* gx_flush_ptr;
static inline void DCStoreRange(void* p, size_t n) {
	gx_flush_ptr = p; gx_flush_bytes = n;
}
static inline void GX_CallDispList(void* p, size_t n) {
	gx_call_ptr = p; gx_call_bytes = n;
}
static inline void GX_Begin(int p, int f, unsigned n) { (void)p; (void)f; (void)n; }
static inline void GX_End(void) {}
static inline void GX_Position3s16(int16_t x, int16_t y, int16_t z) {
	(void)x; (void)y; (void)z;
}
static inline void GX_Color1x8(uint8_t x) { (void)x; }
static inline void GX_Color3u8(uint8_t x, uint8_t y, uint8_t z) {
	(void)x; (void)y; (void)z;
}
static inline void GX_TexCoord2u8(uint8_t x, uint8_t y) { (void)x; (void)y; }
#endif

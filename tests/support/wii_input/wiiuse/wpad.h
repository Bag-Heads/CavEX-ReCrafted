/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef TEST_WII_INPUT_WPAD_H
#define TEST_WII_INPUT_WPAD_H
#include <gccore.h>
/* Consumed libogc API only. Reduced structs are test doubles, not ABI shims. */
enum { WPAD_CHAN_ALL = -1, WPAD_CHAN_0 = 0 };
enum { WPAD_EXP_NONE, WPAD_EXP_NUNCHUK, WPAD_EXP_CLASSIC, WPAD_EXP_GUITARHERO3 };
enum { WPAD_FMT_BTNS_ACC_IR = 2 };
#define WPAD_ERR_NONE 0
#define WPAD_ERR_NO_CONTROLLER -1
#define WPAD_ERR_NOT_READY -2
#define WPAD_DATA_BUTTONS 0x01
#define WPAD_DATA_EXPANSION 0x04
#define WPAD_DATA_IR 0x08
struct ir_t { int valid; float x, y, angle; };
struct test_joystick { float ang, mag; };
typedef struct expansion_t {
	int type;
	struct { struct test_joystick js; } nunchuk;
	struct { struct test_joystick ljs, rjs; float l_shoulder, r_shoulder; } classic;
} expansion_t;
typedef struct {
	int err;
	u32 data_present, battery_level, btns_h, btns_d, btns_u;
	expansion_t exp;
	struct ir_t ir;
} WPADData;
typedef void (*WPADDataCallback)(s32 channel, const WPADData* data);
s32 WPAD_Init(void);
s32 WPAD_Shutdown(void);
s32 WPAD_Probe(s32 channel, u32* type);
s32 WPAD_ReadPending(s32 channel, WPADDataCallback callback);
s32 WPAD_SetDataFormat(s32 channel, s32 format);
s32 WPAD_SetVRes(s32 channel, u32 width, u32 height);
WPADData* WPAD_Data(int channel);
u32 WPAD_ButtonsHeld(int channel);
void WPAD_IR(int channel, struct ir_t* ir);
void WPAD_Expansion(int channel, struct expansion_t* exp);
#define WPAD_BUTTON_2							0x0001
#define WPAD_BUTTON_1							0x0002
#define WPAD_BUTTON_B							0x0004
#define WPAD_BUTTON_A							0x0008
#define WPAD_BUTTON_MINUS						0x0010
#define WPAD_BUTTON_HOME						0x0080
#define WPAD_BUTTON_LEFT						0x0100
#define WPAD_BUTTON_RIGHT						0x0200
#define WPAD_BUTTON_DOWN						0x0400
#define WPAD_BUTTON_UP							0x0800
#define WPAD_BUTTON_PLUS						0x1000
#define WPAD_NUNCHUK_BUTTON_Z					(0x0001<<16)
#define WPAD_NUNCHUK_BUTTON_C					(0x0002<<16)
#define WPAD_CLASSIC_BUTTON_UP					(0x0001u<<16)
#define WPAD_CLASSIC_BUTTON_LEFT				(0x0002u<<16)
#define WPAD_CLASSIC_BUTTON_ZR					(0x0004u<<16)
#define WPAD_CLASSIC_BUTTON_X					(0x0008u<<16)
#define WPAD_CLASSIC_BUTTON_A					(0x0010u<<16)
#define WPAD_CLASSIC_BUTTON_Y					(0x0020u<<16)
#define WPAD_CLASSIC_BUTTON_B					(0x0040u<<16)
#define WPAD_CLASSIC_BUTTON_ZL					(0x0080u<<16)
#define WPAD_CLASSIC_BUTTON_FULL_R				(0x0200u<<16)
#define WPAD_CLASSIC_BUTTON_PLUS				(0x0400u<<16)
#define WPAD_CLASSIC_BUTTON_HOME				(0x0800u<<16)
#define WPAD_CLASSIC_BUTTON_MINUS				(0x1000u<<16)
#define WPAD_CLASSIC_BUTTON_FULL_L				(0x2000u<<16)
#define WPAD_CLASSIC_BUTTON_DOWN				(0x4000u<<16)
#define WPAD_CLASSIC_BUTTON_RIGHT				(0x8000u<<16)
#define WPAD_GUITAR_HERO_3_BUTTON_STRUM_UP		(0x0001<<16)
#define WPAD_GUITAR_HERO_3_BUTTON_YELLOW		(0x0008<<16)
#define WPAD_GUITAR_HERO_3_BUTTON_GREEN			(0x0010<<16)
#define WPAD_GUITAR_HERO_3_BUTTON_BLUE			(0x0020<<16)
#define WPAD_GUITAR_HERO_3_BUTTON_RED			(0x0040<<16)
#define WPAD_GUITAR_HERO_3_BUTTON_ORANGE		(0x0080<<16)
#define WPAD_GUITAR_HERO_3_BUTTON_PLUS			(0x0400<<16)
#define WPAD_GUITAR_HERO_3_BUTTON_MINUS			(0x1000<<16)
#define WPAD_GUITAR_HERO_3_BUTTON_STRUM_DOWN	(0x4000<<16)
#endif

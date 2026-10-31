/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef TEST_WII_INPUT_PAD_H
#define TEST_WII_INPUT_PAD_H
#include <gccore.h>
/* Consumed libogc API only. Masks match ogc/pad.h. */
#define PAD_CHAN0 0
#define PAD_CHAN0_BIT 0x80000000u
#define PAD_BUTTON_LEFT 0x0001
#define PAD_BUTTON_RIGHT 0x0002
#define PAD_BUTTON_DOWN 0x0004
#define PAD_BUTTON_UP 0x0008
#define PAD_TRIGGER_Z 0x0010
#define PAD_TRIGGER_R 0x0020
#define PAD_TRIGGER_L 0x0040
#define PAD_BUTTON_A 0x0100
#define PAD_BUTTON_B 0x0200
#define PAD_BUTTON_X 0x0400
#define PAD_BUTTON_Y 0x0800
#define PAD_BUTTON_START 0x1000
u32 PAD_Init(void);
/* Returns connected channels as (1 << channel), exactly like libogc/pad.c
 * PAD_ScanPads (`connected |= (1<<i)`) -- NOT PAD_CHAN0_BIT >> channel. */
u32 PAD_ScanPads(void);
/* Takes PAD_CHAN0_BIT >> channel bits (libogc/pad.c PAD_Reset). */
u32 PAD_Reset(u32 mask);
u16 PAD_ButtonsDown(int channel);
u16 PAD_ButtonsHeld(int channel);
s8 PAD_StickX(int channel);
s8 PAD_StickY(int channel);
s8 PAD_SubStickX(int channel);
s8 PAD_SubStickY(int channel);
#endif

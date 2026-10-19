/* gba.h - minimal GBA hardware layer (raw registers, no libraries).
 *
 * Built for the real console with devkitARM, or with -DHOST for a PC build
 * where VRAM / palettes / OAM / registers are plain arrays that host/host.c
 * renders like the PPU would (used for testing the game logic).             */
#ifndef GBA_H
#define GBA_H
#include <stdint.h>

typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;

#ifdef HOST
  extern u16 hw_io[0x200];
  extern u16 hw_pal_bg[256], hw_pal_obj[256];
  extern u16 hw_vram[0xC000];
  extern u16 hw_oam[512];
  extern u8  hw_sram[0x10000];
  #define IO16(off)   (hw_io[(off) >> 1])
  #define PAL_BG      ((volatile u16*)hw_pal_bg)
  #define PAL_OBJ     ((volatile u16*)hw_pal_obj)
  #define VRAM16      ((volatile u16*)hw_vram)
  #define OAM16       ((volatile u16*)hw_oam)
  #define SRAM        ((volatile u8*)hw_sram)
#else
  #define IO16(off)   (*(volatile u16*)(0x04000000 + (off)))
  #define PAL_BG      ((volatile u16*)0x05000000)
  #define PAL_OBJ     ((volatile u16*)0x05000200)
  #define VRAM16      ((volatile u16*)0x06000000)
  #define OAM16       ((volatile u16*)0x07000000)
  #define SRAM        ((volatile u8*)0x0E000000)
#endif

/* ---- registers ---- */
#define REG_DISPCNT   IO16(0x000)
#define REG_VCOUNT    IO16(0x006)
#define REG_BG0CNT    IO16(0x008)
#define REG_BG1CNT    IO16(0x00A)
#define REG_BG0HOFS   IO16(0x010)
#define REG_BG0VOFS   IO16(0x012)
#define REG_BG1HOFS   IO16(0x014)
#define REG_BG1VOFS   IO16(0x016)
#define REG_SND1CNT_L IO16(0x060)
#define REG_SND1CNT_H IO16(0x062)
#define REG_SND1CNT_X IO16(0x064)
#define REG_SND4CNT_L IO16(0x078)
#define REG_SND4CNT_H IO16(0x07C)
#define REG_SOUNDCNT_L IO16(0x080)
#define REG_SOUNDCNT_H IO16(0x082)
#define REG_SOUNDCNT_X IO16(0x084)
#define REG_KEYINPUT  IO16(0x130)
#define REG_WAITCNT   IO16(0x204)

#define DCNT_FORCE_BLANK 0x0080
#define DCNT_OBJ_1D      0x0040
#define DCNT_BG0 0x0100
#define DCNT_BG1 0x0200
#define DCNT_OBJ 0x1000

#define KEY_A 0x001
#define KEY_B 0x002
#define KEY_SELECT 0x004
#define KEY_START 0x008
#define KEY_RIGHT 0x010
#define KEY_LEFT 0x020
#define KEY_UP 0x040
#define KEY_DOWN 0x080
#define KEY_R 0x100
#define KEY_L 0x200

/* ---- VRAM helpers (VRAM/OAM/PAL need 16- or 32-bit accesses, never bytes) ---- */
#define BG_TILE_MEM(cb, n)  ((volatile u32*)((volatile u8*)VRAM16 + (cb) * 0x4000 + (n) * 32))
#define OBJ_TILE_MEM(n)     ((volatile u32*)((volatile u8*)VRAM16 + 0x10000 + (n) * 32))
#define SCREENBLOCK(n)      (VRAM16 + (n) * 1024)

static inline void copy32(volatile void *dst, const void *src, u32 words) {
    volatile u32 *d = (volatile u32 *)dst; const u32 *s = (const u32 *)src;
    while (words--) *d++ = *s++;
}
static inline void fill32(volatile void *dst, u32 value, u32 words) {
    volatile u32 *d = (volatile u32 *)dst;
    while (words--) *d++ = value;
}

/* implemented in game.c (GBA) / host.c (PC) */
void hw_wait_vblank(void);
void hw_frame_done(void);

#endif

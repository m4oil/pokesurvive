/* game.h - game state shared between game.c and the host test harness */
#ifndef GAME_H
#define GAME_H
#include "gba.h"
#include "assets_gen.h"

#define MAX_ENEMIES 32
#define MAX_SHOTS   16
#define MAX_GEMS    28
#define MAX_HEARTS  4
#define MAX_ESHOTS  32
#define NUM_BOSSES  3

enum { ST_TITLE, ST_PLAY, ST_LEVELUP, ST_PAUSE, ST_OVER };
enum { UP_POWER, UP_RAPID, UP_SWIFT, UP_VITAL, UP_MULTI, UP_PIERCE, UP_MAGNET, UP_HEAL, UP_COUNT };

typedef struct { s32 x, y, kx, ky; s16 hp; u8 tier, alive, flash, id; } Enemy;
typedef struct { s32 x, y, vx, vy; s16 dmg; u8 life, pierce, hit, alive; } Shot;
typedef struct { s32 x, y; u8 kind, alive, magnet; } Gem;
typedef struct { s32 x, y; u8 alive; } Heart;
typedef struct { s32 x, y, vx, vy; u8 life, alive; } EShot;          /* boss bullet */
typedef struct { s32 x, y, hp, maxhp, dvx, dvy; s16 atk_t, dash_t, timer; u8 alive, flash, idx, state; } Boss;

typedef struct {
    int state;  u32 frame;  u32 rng;
    int sel;                           /* species highlighted on the title screen */
    /* current run */
    int species;  s32 px, py;  int dir_row, dir_flip, anim, moving;
    int hp, maxhp, inv;
    int lvl, xp, xpneed, kills, t;
    int up[UP_COUNT];                  /* upgrade levels */
    int multi, pierce;
    int fire_cd, spawn_cd, elite_cd, nen;
    u8  tier_species[4];
    u8  mult_out[4], mult_in[4];       /* type multipliers (percent) */
    int lu_opt[3], lu_cur;
    int best, result, new_best;
    int gem_next;
    Boss boss; u8 boss_species[NUM_BOSSES]; int next_boss, warn, win;
    EShot es[MAX_ESHOTS];
    Enemy en[MAX_ENEMIES]; Shot sh[MAX_SHOTS]; Gem gem[MAX_GEMS]; Heart ht[MAX_HEARTS];
} Game;

extern Game g;
#ifdef HOST
int game_main(void);
#endif
#endif

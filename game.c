/* PokeSurvivor GBA - a small bullet-heaven for the Game Boy Advance.
 * Fan project, non-commercial. See README.md / CREDITS.md.
 *
 * Hardware use
 *   Mode 0 | BG0 = endless procedural floor (64x64 tile ring buffer, streamed)
 *          | BG1 = HUD / menus (32x32 text layer, shadow copy in RAM)
 *          | OBJ = 1D mapping, 4bpp, 128 sprites
 *   OBJ tiles : 0..15      player frame (streamed whenever dir/frame changes)
 *               64..831    4 enemy tiers x 3 dirs x 4 frames x 16 tiles
 *               832..845   shots, gems, heart, shadow
 *   OBJ pal   : 0 shot colours | 1 player | 2..5 enemy tiers | 6 gems | 7 white flash
 */
#include "game.h"

/* ================================================================ tunables */
#define ES_SPEED            400             /* boss bullet speed, 1/256 px per frame */
#define SHOT_LIFE           70
#define BASE_FIRE_INTERVAL  40
#define PICKUP_RADIUS       30
#define INV_FRAMES          45

/* ============================================================ VRAM layout */
#define SB_FLOOR 16          /* BG0 64x64 map: screenblocks 16..19 */
#define SB_HUD   20          /* BG1 32x32 map */
#define CB_FLOOR 0           /* BG0 tiles in charblock 0 */
#define CB_FONT  1           /* BG1 tiles in charblock 1 */

#define OBJ_PLAYER   0
#define OBJ_ENEMY    64
#define OBJ_MISC     832
#define ENEMY_TILES  192     /* 3 dirs x 4 frames x 16 tiles */

#define PAL_SHOT 0
#define PAL_PLAYER 1
#define PAL_ENEMY0 2
#define PAL_GEM 6
#define PAL_FLASH 7
#define PAL_EBULLET 8

/* BG1 palette banks */
#define TP_WHITE 1
#define TP_HP    2
#define TP_XP    3
#define TP_GOLD  4
#define TP_GREY  5

Game g;

static const u16 BOSS_AT_SEC[NUM_BOSSES] = { 90, 180, 270 };    /* 1:30, 3:00, 4:30 - the last one ends the game */
static const s16 ROT_C[5] = { 256, 255, 250, 243, 234 };        /* cos, sin of m * 6 degrees */
static const s16 ROT_S[5] = { 0, 27, 53, 79, 104 };
static const s16 DIR24[24][2] = {{256,0},{247,66},{222,128},{181,181},{128,222},{66,247},{0,256},{-66,247},{-128,222},{-181,181},{-222,128},{-247,66},
                                 {-256,0},{-247,-66},{-222,-128},{-181,-181},{-128,-222},{-66,-247},{0,-256},{66,-247},{128,-222},{181,-181},{222,-128},{247,-66}};
static int boss_load = -1;                                      /* species whose tiles/palette must be uploaded in VBlank */

/* ================================================================ helpers */
static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static u32 rnd(void) { u32 x = g.rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; g.rng = x; return x; }
static int rnd_n(int n) { return (int)(((rnd() >> 16) * (u32)n) >> 16); }

/* cheap distance estimate (within ~12%) */
static int approx_dist(int dx, int dy) {
    dx = iabs(dx); dy = iabs(dy);
    return dx > dy ? dx + (dy >> 1) : dy + (dx >> 1);
}
/* vector (dx,dy) scaled to length `len` */
static void norm_to(int dx, int dy, int len, int *ox, int *oy) {
    int d = approx_dist(dx, dy);
    if (d == 0) { *ox = 0; *oy = len; return; }
    *ox = dx * len / d; *oy = dy * len / d;
}
static int decay(int v) { return v > 0 ? (v * 7) >> 3 : -((-v * 7) >> 3); }

static int xp_for(int level) { return 3 + 3 * level; }      /* XP needed to leave `level` */

static int type_mult(int atk, int d1, int d2) {
    int m = type_chart[atk][d1];
    if (d2 != 255) m = m * type_chart[atk][d2] / 100;
    return clampi(m, 30, 240);
}

/* ================================================================== input */
static u16 keys, keys_prev, pressed;
static void input_poll(void) {
    keys_prev = keys;
    keys = (u16)(~REG_KEYINPUT & 0x3FF);
    pressed = keys & ~keys_prev;
}

/* ================================================================== sound */
static int sfx_cd;
static void sound_init(void) {
    REG_SOUNDCNT_X = 0x0080;       /* master enable */
    REG_SOUNDCNT_L = 0xFF77;       /* all PSG channels, both speakers, full volume */
    REG_SOUNDCNT_H = 0x0002;       /* PSG at 100% */
}
static void sfx_shoot(void)  { REG_SND4CNT_L = 0x2100; REG_SND4CNT_H = 0x8031; }
static void sfx_hit(void)    { if (sfx_cd > 0) return; sfx_cd = 3; REG_SND4CNT_L = 0x5100; REG_SND4CNT_H = 0x8053; }
static void sfx_hurt(void)   { REG_SND4CNT_L = 0xB200; REG_SND4CNT_H = 0x8074; }
static void sfx_gem(void)    { REG_SND1CNT_L = 0x0008; REG_SND1CNT_H = 0x6280; REG_SND1CNT_X = 0x87A9; }
static void sfx_levelup(void){ REG_SND1CNT_L = 0x0008; REG_SND1CNT_H = 0x7380; REG_SND1CNT_X = 0x87C1; }

/* ================================================================== save */
static void save_load(void) {
    if (SRAM[0] == 'P' && SRAM[1] == 'S' && SRAM[2] == 'V' && SRAM[3] == 1)
        g.best = SRAM[4] | (SRAM[5] << 8) | (SRAM[6] << 16) | (SRAM[7] << 24);
    else g.best = 0;
}
static void save_write(void) {
    SRAM[0] = 'P'; SRAM[1] = 'S'; SRAM[2] = 'V'; SRAM[3] = 1;
    SRAM[4] = g.best & 255; SRAM[5] = (g.best >> 8) & 255;
    SRAM[6] = (g.best >> 16) & 255; SRAM[7] = (g.best >> 24) & 255;
}
#ifndef HOST
/* emulators look for this tag to pick the right save type (32 KB SRAM) */
__attribute__((used)) const char sram_tag[] = "SRAM_V113";
#endif

/* ============================================================== HUD / text */
static u16 hud[32 * 32];
static int hud_dirty;
static int tbg;          /* 0: transparent text, T_PGLYPH: text with panel-coloured background */
static u8 font_lut[128];

static void font_init(void) {
    for (int i = 0; i < 128; i++) font_lut[i] = 0;
    const char *o = FONT_ORDER;
    for (int i = 0; o[i]; i++) font_lut[(int)o[i]] = (u8)i;
    for (int c = 'a'; c <= 'z'; c++) font_lut[c] = font_lut[c - 32];
}
static void hud_clear(void) { tbg = 0; for (int i = 0; i < 32 * 32; i++) hud[i] = 0; hud_dirty = 1; }
static void hud_clear_rows(int r0, int r1) { /* HUD rows are always drawn without a panel */ for (int i = r0 * 32; i < (r1 + 1) * 32; i++) hud[i] = 0; hud_dirty = 1; }
static void put_text(int row, int col, const char *s, int pal) {
    for (; *s && col < 32; s++, col++) hud[row * 32 + col] = (u16)((font_lut[*s & 127] + tbg) | (pal << 12));
    hud_dirty = 1;
}
static void put_text_c(int row, const char *s, int pal) {   /* centred on the 30-column screen */
    int n = 0; while (s[n]) n++;
    put_text(row, (30 - n) / 2, s, pal);
}
static void put_num(int row, int col, int v, int width, int pal) {
    char b[12]; int n = 0;
    if (v < 0) v = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    for (int i = 0; i < width; i++) {
        char c = i < n ? b[i] : ' ';
        hud[row * 32 + col + width - 1 - i] = (u16)((font_lut[(int)c] + tbg) | (pal << 12));
    }
    hud_dirty = 1;
}
static void put_time(int row, int col, int frames, int pal) {
    int s = frames / 60, m = s / 60; s %= 60;
    char b[6] = { (char)('0' + (m / 10) % 10), (char)('0' + m % 10), ':', (char)('0' + s / 10), (char)('0' + s % 10), 0 };
    put_text(row, col, b, pal);
}
static void put_bar(int row, int col, int tiles, int px, int pal) {
    for (int i = 0; i < tiles; i++)
        hud[row * 32 + col + i] = (u16)((T_BAR + clampi(px - i * 8, 0, 8)) | (pal << 12));
    hud_dirty = 1;
}
static void put_panel(int r0, int r1, int c0, int c1) {
    tbg = T_PGLYPH;
    for (int r = r0; r <= r1; r++) for (int c = c0; c <= c1; c++)
        hud[r * 32 + c] = (u16)(T_PANEL | (TP_WHITE << 12));
    hud_dirty = 1;
}

/* ============================================================ BG0: floor */
static int bg_tx, bg_ty;
static u32 hash2(int x, int y) {
    u32 h = (u32)x * 0x9E3779B1u ^ (u32)y * 0x85EBCA6Bu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
static void floor_put(int wx, int wy) {
    u32 h = hash2(wx, wy);
    u32 r = h & 255;
    int t = r < 5 ? 3 : r < 9 ? 4 : r < 12 ? 5 : r < 40 ? 2 : r < 55 ? 6 : r < 130 ? 1 : 0;
    u16 e = (u16)(t | (((h >> 8) & 1) << 10) | (((h >> 9) & 1) << 11));
    int mx = wx & 63, my = wy & 63;
    SCREENBLOCK(SB_FLOOR + ((my >> 5) << 1) + (mx >> 5))[(my & 31) * 32 + (mx & 31)] = e;
}
static void floor_init(int cx, int cy) {
    bg_tx = cx >> 3; bg_ty = cy >> 3;
    for (int wy = bg_ty - 1; wy <= bg_ty + 22; wy++)
        for (int wx = bg_tx - 1; wx <= bg_tx + 32; wx++) floor_put(wx, wy);
}
static void floor_update(int cx, int cy) {
    int tx = cx >> 3, ty = cy >> 3;
    while (bg_tx < tx) { bg_tx++; for (int wy = bg_ty - 1; wy <= bg_ty + 22; wy++) floor_put(bg_tx + 32, wy); }
    while (bg_tx > tx) { bg_tx--; for (int wy = bg_ty - 1; wy <= bg_ty + 22; wy++) floor_put(bg_tx - 1, wy); }
    while (bg_ty < ty) { bg_ty++; for (int wx = bg_tx - 1; wx <= bg_tx + 32; wx++) floor_put(wx, bg_ty + 22); }
    while (bg_ty > ty) { bg_ty--; for (int wx = bg_tx - 1; wx <= bg_tx + 32; wx++) floor_put(wx, bg_ty - 1); }
}

/* ================================================================= sprites */
typedef struct { s16 key; u16 a0, a1, a2; } Spr;
static u16 oam_buf[512];
static int oam_n;
static int cam_x, cam_y;
static int pl_load_species = -1, pl_load_row, pl_load_frame;   /* what is in OBJ tiles 0..15 now */
static int pl_want_species, pl_want_row, pl_want_frame;
static int pal_pending = -1;                                   /* species palette to upload in vblank */
static int title_mode_sprite;                                  /* 1: draw preview sprite instead of the world */

static void oam_push(u16 a0, u16 a1, u16 a2) {
    if (oam_n >= 128) return;
    oam_buf[oam_n * 4 + 0] = a0; oam_buf[oam_n * 4 + 1] = a1; oam_buf[oam_n * 4 + 2] = a2;
    oam_n++;
}
/* a 32x32 / 16x16 / 8x8 / 16x8 sprite at screen top-left (sx, sy) */
static inline u16 spr_a0(int sy, int shape) { return (u16)((sy & 0xFF) | (shape << 14)); }
static inline u16 spr_a1(int sx, int size, int flip) { return (u16)((sx & 0x1FF) | (flip << 12) | (size << 14)); }

static void oam_build(void) {
    static Spr units[MAX_ENEMIES + 3];
    int nu = 0, boss_drawn = 0, boss_flip = 0;
    oam_n = 0;

    if (title_mode_sprite) {
        oam_push(spr_a0(64, 1), spr_a1(112, 0, 0), (u16)((OBJ_MISC + MISC_SHADOW) | (2 << 10) | (PAL_GEM << 12)));
        oam_push(spr_a0(44, 0), spr_a1(104, 2, g.dir_flip), (u16)(OBJ_PLAYER | (2 << 10) | (PAL_PLAYER << 12)));
    } else {
        /* shots on top */
        for (int i = 0; i < MAX_SHOTS; i++) {
            Shot *s = &g.sh[i]; if (!s->alive) continue;
            int sx = (s->x >> 8) - cam_x - 8, sy = (s->y >> 8) - cam_y - 8;
            if (sx < -16 || sx > 240 || sy < -16 || sy > 160) continue;
            int tile = OBJ_MISC + ((g.frame >> 2) & 1 ? MISC_SHOT_B : MISC_SHOT_A);
            oam_push(spr_a0(sy, 0), spr_a1(sx, 1, 0), (u16)(tile | (2 << 10) | (PAL_SHOT << 12)));
        }
        for (int i = 0; i < MAX_ESHOTS; i++) {
            EShot *e = &g.es[i]; if (!e->alive) continue;
            int sx = (e->x >> 8) - cam_x - 8, sy = (e->y >> 8) - cam_y - 8;
            if (sx < -16 || sx > 240 || sy < -16 || sy > 160) continue;
            int tile = OBJ_MISC + ((g.frame >> 2) & 1 ? MISC_SHOT_A : MISC_SHOT_B);
            oam_push(spr_a0(sy, 0), spr_a1(sx, 1, 0), (u16)(tile | (2 << 10) | (PAL_EBULLET << 12)));
        }
        /* enemies + player, lower on screen = drawn on top */
        for (int i = 0; i < MAX_ENEMIES; i++) {
            Enemy *e = &g.en[i]; if (!e->alive) continue;
            int sx = (e->x >> 8) - cam_x - 16, sy = (e->y >> 8) - cam_y - 24;
            if (sx < -32 || sx > 240 || sy < -32 || sy > 160) continue;
            int vx = g.px - e->x, vy = g.py - e->y;                 /* faces the player */
            int slot = (iabs(vy) > 2 * iabs(vx)) ? (vy > 0 ? 0 : 2) : 1;
            int frame = ((g.frame >> 3) + e->id) & 3;
            int tile = OBJ_ENEMY + e->tier * ENEMY_TILES + (slot * 4 + frame) * 16;
            int pal = e->flash ? PAL_FLASH : PAL_ENEMY0 + e->tier;
            Spr *u = &units[nu++];
            u->key = (s16)(e->y >> 8);
            u->a0 = spr_a0(sy, 0); u->a1 = spr_a1(sx, 2, (slot == 1 && vx < 0));
            u->a2 = (u16)(tile | (2 << 10) | (pal << 12));
        }
        if (g.boss.alive) {                                     /* 32x32 sprite scaled 2x with an affine, double-size OBJ */
            Boss *b = &g.boss;
            int cx = (b->x >> 8) - cam_x, cy = (b->y >> 8) - cam_y - 16;   /* sprite centre; feet stay on b->y */
            int vx = g.px - b->x, vy = g.py - b->y;
            int slot = (iabs(vy) > 2 * iabs(vx)) ? (vy > 0 ? 0 : 2) : 1;
            int frame = (g.frame >> 3) & 3;
            int tile = OBJ_ENEMY + 3 * ENEMY_TILES + (slot * 4 + frame) * 16;
            int pal = b->flash ? PAL_FLASH : PAL_ENEMY0 + 3;
            Spr *u = &units[nu++];
            u->key = (s16)(b->y >> 8);
            u->a0 = (u16)(((cy - 32) & 0xFF) | 0x300);              /* affine + double size, square */
            u->a1 = (u16)(((cx - 32) & 0x1FF) | (2 << 14));         /* affine group 0, 32x32 */
            u->a2 = (u16)(tile | (2 << 10) | (pal << 12));
            boss_flip = (slot == 1 && vx < 0);
            boss_drawn = 1;
        }
        if (g.state != ST_OVER || g.result == 1) {
            if (!(g.inv > 0 && (g.inv & 4))) {
                int sx = (g.px >> 8) - cam_x - 16, sy = (g.py >> 8) - cam_y - 24;
                Spr *u = &units[nu++];
                u->key = (s16)(g.py >> 8);
                u->a0 = spr_a0(sy, 0); u->a1 = spr_a1(sx, 2, g.dir_flip);
                u->a2 = (u16)(OBJ_PLAYER | (2 << 10) | (PAL_PLAYER << 12));
            }
        }
        for (int i = 1; i < nu; i++) {                              /* insertion sort, descending y */
            Spr t = units[i]; int j = i - 1;
            while (j >= 0 && units[j].key < t.key) { units[j + 1] = units[j]; j--; }
            units[j + 1] = t;
        }
        for (int i = 0; i < nu; i++) oam_push(units[i].a0, units[i].a1, units[i].a2);
        /* pickups under the units */
        for (int i = 0; i < MAX_HEARTS; i++) {
            Heart *h = &g.ht[i]; if (!h->alive) continue;
            int sx = (h->x >> 8) - cam_x - 4, sy = (h->y >> 8) - cam_y - 8;
            if (sx < -8 || sx > 240 || sy < -8 || sy > 160) continue;
            oam_push(spr_a0(sy, 0), spr_a1(sx, 0, 0), (u16)((OBJ_MISC + MISC_HEART) | (2 << 10) | (PAL_GEM << 12)));
        }
        for (int i = 0; i < MAX_GEMS; i++) {
            Gem *m = &g.gem[i]; if (!m->alive) continue;
            int sx = (m->x >> 8) - cam_x - 4, sy = (m->y >> 8) - cam_y - 6;
            if (sx < -8 || sx > 240 || sy < -8 || sy > 160) continue;
            oam_push(spr_a0(sy, 0), spr_a1(sx, 0, 0), (u16)((OBJ_MISC + MISC_GEM_S + m->kind) | (2 << 10) | (PAL_GEM << 12)));
        }
        if (g.state != ST_OVER || g.result == 1) {
            int sx = (g.px >> 8) - cam_x - 8, sy = (g.py >> 8) - cam_y - 4;
            oam_push(spr_a0(sy, 1), spr_a1(sx, 0, 0), (u16)((OBJ_MISC + MISC_SHADOW) | (2 << 10) | (PAL_GEM << 12)));
        }
    }
    for (int i = oam_n; i < 128; i++) { oam_buf[i * 4] = 0x0200; oam_buf[i * 4 + 1] = 0; oam_buf[i * 4 + 2] = 0; }
    /* affine group 0 lives in the 4th halfword of OAM entries 0..3: pa, pb, pc, pd (8.8) - 128 = magnify 2x */
    oam_buf[3] = (u16)(boss_drawn ? (boss_flip ? -128 : 128) : 256); oam_buf[7] = 0; oam_buf[11] = 0; oam_buf[15] = (u16)(boss_drawn ? 128 : 256);
}

/* ============================================================== commit (VBlank) */
static void commit(void) {
    copy32(OAM16, oam_buf, 256);
    if (pal_pending >= 0) {
        for (int i = 0; i < 16; i++) PAL_OBJ[PAL_PLAYER * 16 + i] = species_pal[pal_pending][i];
        pal_pending = -1;
    }
    if (pl_want_species != pl_load_species || pl_want_row != pl_load_row || pl_want_frame != pl_load_frame) {
        copy32(OBJ_TILE_MEM(OBJ_PLAYER), species_tiles[pl_want_species][pl_want_row][pl_want_frame], 128);
        pl_load_species = pl_want_species; pl_load_row = pl_want_row; pl_load_frame = pl_want_frame;
    }
    if (boss_load >= 0) {
        static const int rows[3] = { 0, 2, 4 };
        for (int d = 0; d < 3; d++) for (int f = 0; f < 4; f++)
            copy32(OBJ_TILE_MEM(OBJ_ENEMY + 3 * ENEMY_TILES + (d * 4 + f) * 16), species_tiles[boss_load][rows[d]][f], 128);
        for (int i = 0; i < 16; i++) PAL_OBJ[(PAL_ENEMY0 + 3) * 16 + i] = species_pal[boss_load][i];
        const Species *bs = &species[boss_load];
        for (int i = 0; i < 3; i++) PAL_OBJ[PAL_EBULLET * 16 + 1 + i] = type_color[bs->t1][i];
        boss_load = -1;
    }
    if (hud_dirty) { copy32(SCREENBLOCK(SB_HUD), hud, 512); hud_dirty = 0; }
    REG_BG0HOFS = (u16)(cam_x & 511); REG_BG0VOFS = (u16)(cam_y & 511);
    floor_update(cam_x, cam_y);
}

/* ============================================================== graphics init */
static void gfx_init(void) {
    REG_DISPCNT = DCNT_FORCE_BLANK;
    copy32(PAL_BG, bg_pal, 128);
    copy32(PAL_OBJ, obj_misc_pal, 128);
    copy32(BG_TILE_MEM(CB_FLOOR, 0), floor_tiles, FLOOR_TILES * 8);
    copy32(BG_TILE_MEM(CB_FONT, 0), font_tiles, BG_FONT_TILES * 8);
    copy32(OBJ_TILE_MEM(OBJ_MISC), misc_tiles, MISC_TILES * 8);
    fill32(SCREENBLOCK(SB_HUD), 0, 512);
    REG_BG0CNT = (u16)(3 | (CB_FLOOR << 2) | (SB_FLOOR << 8) | (3 << 14));
    REG_BG1CNT = (u16)(1 | (CB_FONT << 2) | (SB_HUD << 8) | (0 << 14));
    REG_BG1HOFS = 0; REG_BG1VOFS = 0;
    for (int i = 0; i < 128; i++) { oam_buf[i * 4] = 0x0200; oam_buf[i * 4 + 1] = 0; oam_buf[i * 4 + 2] = 0; }
    copy32(OAM16, oam_buf, 256);
    font_init();
}
static void display_on(void)  { REG_DISPCNT = DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D; }
static void display_off(void) { REG_DISPCNT = DCNT_FORCE_BLANK; }

/* ================================================================== title */
static void set_preview(int sp) {
    pl_want_species = sp; pl_want_row = 0; pl_want_frame = 0;
    pal_pending = sp;
}
static void title_draw(void) {
    const Species *s = &species[g.sel];
    hud_clear();
    put_text_c(1, "POKESURVIVOR", TP_GOLD);
    put_text_c(2, "GBA DEMO", TP_WHITE);
    put_text(7, 3, "<", TP_WHITE); put_text(7, 26, ">", TP_WHITE);
    put_text_c(10, s->name, TP_WHITE);
    char tb[24]; int n = 0;
    for (const char *p = type_name[s->t1]; *p; p++) tb[n++] = *p;
    if (s->t2 != 255) { tb[n++] = '/'; for (const char *p = type_name[s->t2]; *p; p++) tb[n++] = *p; }
    tb[n] = 0;
    put_text_c(11, tb, TP_GOLD);
    put_text(13, 2, "HP", TP_GREY);   put_num(13, 5, s->hp, 3, TP_WHITE);
    put_text(13, 11, "SPD", TP_GREY); put_num(13, 15, s->spd, 3, TP_WHITE);
    put_text(13, 20, "ATK", TP_GREY); put_num(13, 24, s->atk, 3, TP_WHITE);
    put_num(15, 12, g.sel + 1, 2, TP_GREY); put_text(15, 14, "/", TP_GREY); put_num(15, 15, NUM_SPECIES, 2, TP_GREY);
    put_text_c(16, "A: START", TP_WHITE);
    put_text(18, 2, "BEST", TP_GOLD); put_time(18, 7, g.best, TP_WHITE);
    put_text(18, 16, "L R: CHANGE", TP_GREY);
    put_text(19, 1, "SPRITES SPRITECOLLAB CC BY-NC", TP_GREY);
}
static void title_enter(void) {
    g.state = ST_TITLE; title_mode_sprite = 1;
    g.dir_flip = 0; g.dir_row = 0;
    display_off();
    cam_x = 0; cam_y = 0; floor_init(cam_x, cam_y);
    set_preview(g.sel);
    title_draw();
    display_on();
}
static void title_update(void) {
    int d = 0;
    if (pressed & KEY_RIGHT) d = 1;
    if (pressed & KEY_LEFT) d = -1;
    if (pressed & KEY_R) d = 5;
    if (pressed & KEY_L) d = -5;
    if (d) {
        g.sel = (g.sel + d + NUM_SPECIES * 2) % NUM_SPECIES;
        set_preview(g.sel); title_draw();
        g.rng ^= g.frame * 2654435761u;
    }
    pl_want_row = 0; pl_want_frame = (g.frame >> 3) & 3;
    cam_x = (int)(g.frame >> 1); cam_y = (int)(g.frame >> 2);
}

/* ============================================================ run set-up */
static void run_start(int sp) {
    g.rng ^= g.frame * 2654435761u; if (!g.rng) g.rng = 1;
    g.species = sp;
    const Species *ps = &species[sp];
    /* pick 4 enemy species, weakest -> strongest, never the player's own */
    u8 pool[NUM_SPECIES]; int n = 0;
    for (int i = 0; i < NUM_SPECIES; i++) if (species_by_bst[i] != sp) pool[n++] = species_by_bst[i];
    int cut[5] = { 0, n * 40 / 100, n * 70 / 100, n * 90 / 100, n };
    for (int t = 0; t < 4; t++) g.tier_species[t] = pool[cut[t] + rnd_n(cut[t + 1] - cut[t])];

    /* the 3 bosses: drawn from the 4 strongest species, weakest fight first */
    { int top[4], m = n - cut[3]; for (int i = 0; i < m && i < 4; i++) top[i] = pool[cut[3] + i];
      for (int k = 0; k < NUM_BOSSES; k++) { int j = rnd_n(m - k); g.boss_species[k] = (u8)top[j]; top[j] = top[m - 1 - k]; }
      for (int a = 0; a < NUM_BOSSES; a++) for (int b = a + 1; b < NUM_BOSSES; b++)
          if (species[g.boss_species[b]].bst < species[g.boss_species[a]].bst) { u8 t = g.boss_species[a]; g.boss_species[a] = g.boss_species[b]; g.boss_species[b] = t; } }
    g.tier_species[3] = g.boss_species[0];

    display_off();
    static const int dir_rows[3] = { 0, 2, 4 };                     /* Down, Right, Up */
    for (int t = 0; t < 4; t++) {
        const Species *es = &species[g.tier_species[t]];
        if (t < 3) for (int d = 0; d < 3; d++) for (int f = 0; f < 4; f++)
            copy32(OBJ_TILE_MEM(OBJ_ENEMY + t * ENEMY_TILES + (d * 4 + f) * 16), species_tiles[g.tier_species[t]][dir_rows[d]][f], 128);
        for (int i = 0; i < 16; i++) PAL_OBJ[(PAL_ENEMY0 + t) * 16 + i] = species_pal[g.tier_species[t]][i];
        g.mult_out[t] = (u8)type_mult(ps->t1, es->t1, es->t2);
        g.mult_in[t]  = (u8)type_mult(es->t1, ps->t1, ps->t2);
    }
    for (int i = 0; i < 16; i++) PAL_OBJ[PAL_PLAYER * 16 + i] = species_pal[sp][i];
    PAL_OBJ[PAL_SHOT * 16 + 1] = type_color[ps->t1][0];
    PAL_OBJ[PAL_SHOT * 16 + 2] = type_color[ps->t1][1];
    PAL_OBJ[PAL_SHOT * 16 + 3] = type_color[ps->t1][2];

    for (int i = 0; i < MAX_ENEMIES; i++) g.en[i].alive = 0;
    for (int i = 0; i < MAX_SHOTS; i++) g.sh[i].alive = 0;
    for (int i = 0; i < MAX_GEMS; i++) g.gem[i].alive = 0;
    for (int i = 0; i < MAX_HEARTS; i++) g.ht[i].alive = 0;
    for (int i = 0; i < MAX_ESHOTS; i++) g.es[i].alive = 0;
    g.boss.alive = 0; g.next_boss = 0; g.warn = 0; g.win = 0;
    for (int i = 0; i < UP_COUNT; i++) g.up[i] = 0;
    g.px = 0; g.py = 0; g.dir_row = 0; g.dir_flip = 0; g.anim = 0; g.moving = 0;
    g.maxhp = ps->hp; g.hp = g.maxhp; g.inv = 0;
    g.lvl = 1; g.xp = 0; g.xpneed = xp_for(1); g.kills = 0; g.t = 0;
    g.multi = 1; g.pierce = 0;
    g.fire_cd = 30; g.spawn_cd = 20; g.nen = 0; g.gem_next = 0;
    g.result = 0; g.new_best = 0;

    title_mode_sprite = 0;
    pl_want_species = sp; pl_want_row = 0; pl_want_frame = 0; pl_load_species = -1;
    cam_x = (g.px >> 8) - 120; cam_y = (g.py >> 8) - 88;
    floor_init(cam_x, cam_y);
    hud_clear();
    g.state = ST_PLAY;
    display_on();
}

/* ============================================================ world logic */
static void spawn_gem(s32 x, s32 y, int kind) {
    Gem *m = &g.gem[g.gem_next]; g.gem_next = (g.gem_next + 1) % MAX_GEMS;
    m->x = x; m->y = y; m->kind = (u8)kind; m->alive = 1; m->magnet = 0;
}
static void spawn_heart(s32 x, s32 y) {
    for (int i = 0; i < MAX_HEARTS; i++) if (!g.ht[i].alive) { g.ht[i].x = x; g.ht[i].y = y; g.ht[i].alive = 1; return; }
}
static int enemy_hp_for(int tier, int tsec) {
    static const u8 mul[4] = { 1, 2, 3, 14 };
    int base = species[g.tier_species[tier]].bst / 20;
    return base * mul[tier] * (100 + tsec / 3) / 100;
}
static void spawn_enemy(int tier) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &g.en[i]; if (e->alive) continue;
        int sx, sy, s = rnd_n(4);
        if (s < 2) { sx = (s == 0 ? 140 : -140) + (s == 0 ? 1 : -1) * rnd_n(24); sy = rnd_n(220) - 110; }
        else       { sy = (s == 2 ? 112 : -112) + (s == 2 ? 1 : -1) * rnd_n(24); sx = rnd_n(300) - 150; }
        e->x = g.px + (sx << 8); e->y = g.py + (sy << 8); e->kx = e->ky = 0;
        e->tier = (u8)tier; e->alive = 1; e->flash = 0; e->id = (u8)i;
        e->hp = (s16)enemy_hp_for(tier, g.t / 60);
        g.nen++;
        return;
    }
}
static void kill_enemy(Enemy *e) {
    e->alive = 0; g.nen--; g.kills++;
    if (e->tier == 3) {
        for (int k = 0; k < 4; k++) spawn_gem(e->x + ((rnd_n(33) - 16) << 8), e->y + ((rnd_n(21) - 10) << 8), 2);
        spawn_heart(e->x, e->y);
    } else {
        spawn_gem(e->x, e->y - (4 << 8), e->tier == 0 ? 0 : 1);
        if (rnd_n(100) < (e->tier ? 4 : 2)) spawn_heart(e->x + (6 << 8), e->y);
    }
}

static void update_spawner(void) {
    int tsec = g.t / 60;
    int cap = imin(MAX_ENEMIES - 2, 16 + tsec / 4);
    if (g.boss.alive) cap = 5;                 /* boss fights are 1-on-few: the swarm thins out */
    int interval = imax(8, 26 - tsec / 12);
    if (--g.spawn_cd <= 0) {
        g.spawn_cd = interval;
        if (g.nen < cap) {
            int r = rnd_n(100), tier;
            if (tsec < 60)        tier = 0;
            else if (tsec < 150)  tier = r < 60 ? 0 : 1;
            else if (tsec < 240)  tier = r < 30 ? 0 : (r < 70 ? 1 : 2);
            else                  tier = r < 15 ? 0 : (r < 50 ? 1 : 2);
            spawn_enemy(tier);
        }
    }
}


/* ================================================================ bosses */
static void hurt_player(int dmg) {
    g.hp -= imax(1, dmg);
    g.inv = INV_FRAMES;
    sfx_hurt();
}
static void es_fire(s32 x, s32 y, int vx, int vy) {
    for (int i = 0; i < MAX_ESHOTS; i++) {
        EShot *e = &g.es[i]; if (e->alive) continue;
        e->x = x; e->y = y; e->vx = vx; e->vy = vy; e->life = 170; e->alive = 1;
        return;
    }
}
static void boss_start(void) {
    int i = g.next_boss++;
    int sp = g.boss_species[i];
    const Species *bs = &species[sp], *ps = &species[g.species];
    static const u8 hp_mul[NUM_BOSSES] = { 2, 4, 7 };
    Boss *b = &g.boss;
    g.tier_species[3] = (u8)sp;
    g.mult_out[3] = (u8)type_mult(ps->t1, bs->t1, bs->t2);
    g.mult_in[3]  = (u8)type_mult(bs->t1, ps->t1, ps->t2);
    b->alive = 1; b->idx = (u8)i; b->flash = 0; b->state = 0;
    b->maxhp = bs->bst * hp_mul[i]; b->hp = b->maxhp;
    b->atk_t = 70; b->dash_t = 240; b->timer = 0;
    b->x = g.px + ((rnd_n(120) - 60) << 8); b->y = g.py - (130 << 8);
    for (int k = 0; k < MAX_ESHOTS; k++) g.es[k].alive = 0;
    g.warn = 150; boss_load = sp;
    sfx_levelup();
}
static void boss_defeat(void) {
    Boss *b = &g.boss;
    b->alive = 0; g.kills += 5;
    for (int k = 0; k < MAX_ESHOTS; k++) g.es[k].alive = 0;
    for (int k = 0; k < 8; k++) spawn_gem(b->x + ((rnd_n(61) - 30) << 8), b->y + ((rnd_n(41) - 20) << 8), 2);
    spawn_heart(b->x - (10 << 8), b->y); spawn_heart(b->x + (10 << 8), b->y);
    sfx_levelup();
    if (b->idx == NUM_BOSSES - 1) g.win = 1;
}
static void boss_aimed(int n) {
    Boss *b = &g.boss;
    int cx = b->x, cy = b->y - (16 << 8);
    int nx, ny; norm_to(g.px - cx, (g.py - (8 << 8)) - cy, 256, &nx, &ny);
    for (int k = 0; k < n; k++) {
        int m = 2 * k - (n - 1), am = iabs(m);
        int c = ROT_C[am], sn = m < 0 ? -ROT_S[am] : ROT_S[am];
        int rx = (nx * c - ny * sn) >> 8, ry = (nx * sn + ny * c) >> 8;
        es_fire(cx, cy, rx * ES_SPEED / 256, ry * ES_SPEED / 256);
    }
}
static void boss_ring(int odd) {
    Boss *b = &g.boss;
    int speed = ES_SPEED * 3 / 4;
    for (int k = odd; k < 24; k += 2)
        es_fire(b->x, b->y - (16 << 8), DIR24[k][0] * speed / 256, DIR24[k][1] * speed / 256);
}
static void update_boss(void) {
    Boss *b = &g.boss; if (!b->alive) return;
    int idx = b->idx;
    if (b->flash) b->flash--;
    int dx = g.px - b->x, dy = g.py - b->y, ad = approx_dist(dx, dy);
    if (b->state == 0) {                                            /* stalk the player and shoot */
        int sp = 120 + 30 * idx;
        if (ad > (58 << 8)) { b->x += dx * sp / ad; b->y += dy * sp / ad; }
        if (--b->atk_t <= 0) {
            static const u8 period[NUM_BOSSES] = { 105, 85, 70 };
            int per = period[idx];
            if (idx == 2 && b->hp * 2 < b->maxhp) per = per * 2 / 3;   /* final boss enrages at 50% */
            b->atk_t = (s16)per; b->timer++;
            if (!(b->timer & 1)) boss_aimed(idx == 2 ? 5 : 3);
            else boss_ring((b->timer >> 1) & 1);
            sfx_shoot();
        }
        if (idx >= 1 && --b->dash_t <= 0) {                         /* bosses 2 and 3 also charge */
            b->state = 1; b->dash_t = 40;
            norm_to(dx, dy, (idx == 2 ? 900 : 780), &b->dvx, &b->dvy);
        }
    } else if (b->state == 1) {                                     /* wind-up: blinks white */
        if (g.frame & 4) b->flash = 2;
        if (--b->dash_t <= 0) { b->state = 2; b->dash_t = 34; }
    } else {                                                        /* dash */
        b->x += b->dvx; b->y += b->dvy;
        if (--b->dash_t <= 0) { b->state = 0; b->dash_t = (s16)(270 - idx * 50); }
    }
    if (idx == 2 && b->hp * 2 < b->maxhp && (g.t % 220) == 0 && g.nen < 18) spawn_enemy(2);   /* enraged: calls reinforcements */
    if (g.inv == 0) {                                               /* touching the boss hurts */
        int cx = (b->x - g.px) >> 8, cy = ((b->y - (14 << 8)) - (g.py - (8 << 8))) >> 8;
        if (cx * cx + cy * cy < 22 * 22) hurt_player((10 + 4 * idx) * g.mult_in[3] / 100);
    }
}
static void update_eshots(void) {
    for (int i = 0; i < MAX_ESHOTS; i++) {
        EShot *e = &g.es[i]; if (!e->alive) continue;
        e->x += e->vx; e->y += e->vy;
        if (--e->life == 0) { e->alive = 0; continue; }
        if (g.inv == 0) {
            int dx = (e->x - g.px) >> 8, dy = (e->y - (g.py - (8 << 8))) >> 8;
            if (dx * dx + dy * dy < 9 * 9) {
                hurt_player((5 + 3 * g.boss.idx) * g.mult_in[3] / 100);
                e->alive = 0;
            }
        }
    }
}

static void update_enemies(void) {
    int tsec = g.t / 60;
    int spd[4];
    for (int t = 0; t < 4; t++) {
        int s = species[g.tier_species[t]].spd * 14 >> 3;
        spd[t] = s + s * imin(tsec, 300) / 900;
    }
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &g.en[i]; if (!e->alive) continue;
        if (e->flash) e->flash--;
        int dx = g.px - e->x, dy = g.py - e->y;
        int ad = approx_dist(dx, dy);
        if (ad > (300 << 8)) { e->alive = 0; g.nen--; continue; }
        if (ad > (6 << 8)) {
            e->x += dx * spd[e->tier] / ad;
            e->y += dy * spd[e->tier] / ad;
        }
        e->x += e->kx; e->y += e->ky; e->kx = decay(e->kx); e->ky = decay(e->ky);
    }
    /* soft separation so they don't stack into one blob */
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *a = &g.en[i]; if (!a->alive) continue;
        for (int j = i + 1; j < MAX_ENEMIES; j++) {
            Enemy *b = &g.en[j]; if (!b->alive) continue;
            int dx = a->x - b->x, dy = a->y - b->y;
            if (iabs(dx) < (10 << 8) && iabs(dy) < (7 << 8)) {
                if (iabs(dx) >= iabs(dy)) { int p = dx >= 0 ? 48 : -48; a->x += p; b->x -= p; }
                else                      { int p = dy >= 0 ? 48 : -48; a->y += p; b->y -= p; }
            }
        }
    }
    /* contact damage */
    if (g.inv > 0) { g.inv--; return; }
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &g.en[i]; if (!e->alive) continue;
        int dx = (e->x - g.px) >> 8, dy = (e->y - g.py) >> 8;
        if (dx * dx + dy * dy < 12 * 12) {
            static const u8 base[4] = { 4, 7, 10, 16 };
            int d = (base[e->tier] + tsec / 60) * g.mult_in[e->tier] / 100;
            g.hp -= imax(1, d);
            g.inv = INV_FRAMES;
            sfx_hurt();
            break;
        }
    }
}

static int fire_interval(void) {
    return imax(10, BASE_FIRE_INTERVAL * imax(28, 100 - 9 * g.up[UP_RAPID]) / 100);
}
static void auto_fire(void) {
    if (--g.fire_cd > 0) return;
    int best = -1, bd = 130;
    int cx = g.px, cy = g.py - (10 << 8);
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &g.en[i]; if (!e->alive) continue;
        int d = approx_dist((e->x - cx) >> 8, ((e->y - (8 << 8)) - cy) >> 8);
        if (d < bd) { bd = d; best = i; }
    }
    int tx = 0, ty = 0;
    if (best >= 0) { tx = g.en[best].x; ty = g.en[best].y - (8 << 8); }
    if (g.boss.alive) {
        int d = approx_dist((g.boss.x - cx) >> 8, ((g.boss.y - (16 << 8)) - cy) >> 8);
        if (d < 150) { best = 0; tx = g.boss.x; ty = g.boss.y - (16 << 8); }        /* boss in range: always the priority target */
    }
    if (best < 0) { g.fire_cd = 0; return; }
    g.fire_cd = fire_interval();
    int nx, ny;
    norm_to(tx - cx, ty - cy, 256, &nx, &ny);
    int dmg = imax(1, 10 * species[g.species].atk * (100 + 25 * g.up[UP_POWER]) / 10000);
    for (int k = 0; k < g.multi; k++) {
        int m = 2 * k - (g.multi - 1), am = iabs(m);
        int c = ROT_C[am], s = m < 0 ? -ROT_S[am] : ROT_S[am];
        int rx = (nx * c - ny * s) >> 8, ry = (nx * s + ny * c) >> 8;
        for (int i = 0; i < MAX_SHOTS; i++) {
            Shot *sh = &g.sh[i]; if (sh->alive) continue;
            sh->x = cx; sh->y = cy;
            sh->vx = rx * 3; sh->vy = ry * 3;                      /* 256 = 1 px -> 3 px/frame */
            sh->dmg = (s16)dmg; sh->life = SHOT_LIFE; sh->pierce = (u8)g.pierce; sh->hit = 0; sh->alive = 1;
            break;
        }
    }
    sfx_shoot();
}

static void update_shots(void) {
    for (int i = 0; i < MAX_SHOTS; i++) {
        Shot *s = &g.sh[i]; if (!s->alive) continue;
        s->x += s->vx; s->y += s->vy;
        if (--s->life == 0) { s->alive = 0; continue; }
        for (int j = 0; j < MAX_ENEMIES; j++) {
            Enemy *e = &g.en[j]; if (!e->alive || s->hit == j + 1) continue;
            int dx = (e->x - s->x) >> 8, dy = ((e->y - (8 << 8)) - s->y) >> 8;
            if (dx * dx + dy * dy >= 13 * 13) continue;
            int d = imax(1, s->dmg * g.mult_out[e->tier] / 100);
            e->hp -= d; e->flash = 4;
            int kd = e->tier == 3 ? 3 : 1;
            e->kx += (s->vx >> 1) / kd; e->ky += (s->vy >> 1) / kd;
            sfx_hit();
            if (e->hp <= 0) kill_enemy(e);
            s->hit = (u8)(j + 1);
            if (s->pierce) s->pierce--; else s->alive = 0;
            break;
        }
        if (s->alive && g.boss.alive && s->hit != 255) {
            Boss *b = &g.boss;
            int dx = (b->x - s->x) >> 8, dy = ((b->y - (16 << 8)) - s->y) >> 8;
            if (dx * dx + dy * dy < 24 * 24) {
                b->hp -= imax(1, s->dmg * g.mult_out[3] / 100);
                b->flash = 3; sfx_hit();
                s->hit = 255;
                if (s->pierce) s->pierce--; else s->alive = 0;
                if (b->hp <= 0) boss_defeat();
            }
        }
    }
}

static void update_pickups(void) {
    int radius = PICKUP_RADIUS + 14 * g.up[UP_MAGNET];
    for (int i = 0; i < MAX_GEMS; i++) {
        Gem *m = &g.gem[i]; if (!m->alive) continue;
        int dx = g.px - m->x, dy = (g.py - (4 << 8)) - m->y;
        int d = approx_dist(dx, dy);
        if (!m->magnet && d < (radius << 8)) m->magnet = 1;
        if (m->magnet) {
            if (d < (8 << 8)) {
                static const u8 val[3] = { 1, 3, 12 };
                g.xp += val[m->kind]; m->alive = 0; sfx_gem();
            } else {
                m->x += dx * (4 << 8) / d; m->y += dy * (4 << 8) / d;
            }
        }
    }
    for (int i = 0; i < MAX_HEARTS; i++) {
        Heart *h = &g.ht[i]; if (!h->alive) continue;
        int dx = (h->x - g.px) >> 8, dy = (h->y - g.py) >> 8;
        if (dx * dx + dy * dy < 14 * 14) {
            g.hp = imin(g.maxhp, g.hp + g.maxhp / 4);
            h->alive = 0; sfx_gem();
        }
    }
}

static void update_player(void) {
    int dx = ((keys & KEY_RIGHT) ? 1 : 0) - ((keys & KEY_LEFT) ? 1 : 0);
    int dy = ((keys & KEY_DOWN) ? 1 : 0) - ((keys & KEY_UP) ? 1 : 0);
    int spd = species[g.species].spd * 3;
    spd += spd * 8 * g.up[UP_SWIFT] / 100;
    g.moving = (dx || dy);
    if (g.moving) {
        int vx = dx * spd, vy = dy * spd;
        if (dx && dy) { vx = (vx * 181) / 256; vy = (vy * 181) / 256; }
        g.px += vx; g.py += vy;
        /* sheet rows: 0 Down, 1 DownRight, 2 Right, 3 UpRight, 4 Up; left side = h-flip */
        int ax = iabs(dx);
        g.dir_flip = dx < 0;
        if (dy > 0)      g.dir_row = ax ? 1 : 0;
        else if (dy < 0) g.dir_row = ax ? 3 : 4;
        else             g.dir_row = 2;
        g.anim++;
    } else g.anim = 0;
    pl_want_species = g.species; pl_want_row = g.dir_row;
    pl_want_frame = g.moving ? ((g.anim >> 3) & 3) : 0;
}

/* ============================================================== level up */
static const char *UP_NAME[UP_COUNT] = { "POWER UP", "RAPID FIRE", "SWIFT FEET", "VITALITY", "MULTI SHOT", "PIERCE", "MAGNET", "RECOVER" };
static const char *UP_DESC[UP_COUNT] = { "DAMAGE +25%", "SHOOT FASTER", "MOVE SPEED +8%", "MAX HP +20 AND HEAL", "ONE MORE SHOT", "HIT ONE MORE FOE", "PICKUP RANGE UP", "HEAL 40% HP" };

static int up_available(int id) {
    switch (id) {
        case UP_POWER:  return g.up[id] < 8;
        case UP_RAPID:  return g.up[id] < 8;
        case UP_SWIFT:  return g.up[id] < 6;
        case UP_MULTI:  return g.multi < 5;
        case UP_PIERCE: return g.pierce < 4;
        case UP_MAGNET: return g.up[id] < 5;
        default:        return 1;
    }
}
static void levelup_draw(void) {
    hud_clear();
    put_panel(4, 17, 2, 27);
    put_text_c(5, "LEVEL UP!", TP_GOLD);
    for (int i = 0; i < 3; i++) {
        int id = g.lu_opt[i], sel = (i == g.lu_cur);
        int row = 8 + i * 3;
        if (sel) put_text(row, 4, ">", TP_GOLD);
        put_text(row, 6, UP_NAME[id], sel ? TP_GOLD : TP_WHITE);
        put_text(row + 1, 6, UP_DESC[id], TP_GREY);
    }
}
static void levelup_enter(void) {
    int pool[UP_COUNT], n = 0;
    for (int i = 0; i < UP_COUNT; i++) if (up_available(i)) pool[n++] = i;
    while (n < 3) pool[n++] = UP_HEAL;                    /* everything maxed: only heals left */
    for (int k = 0; k < 3; k++) {
        int j = rnd_n(n - k);
        g.lu_opt[k] = pool[j]; pool[j] = pool[n - 1 - k];
    }
    g.lu_cur = 0;
    g.state = ST_LEVELUP;
    sfx_levelup();
    levelup_draw();
}
static void levelup_apply(int id) {
    switch (id) {
        case UP_VITAL: g.maxhp += 20; g.hp = imin(g.maxhp, g.hp + 20); g.up[id]++; break;
        case UP_MULTI: g.multi++; g.up[id]++; break;
        case UP_PIERCE: g.pierce++; g.up[id]++; break;
        case UP_HEAL:  g.hp = imin(g.maxhp, g.hp + g.maxhp * 40 / 100); break;
        default:       g.up[id]++; break;
    }
}
static void levelup_update(void) {
    if (pressed & KEY_DOWN) { g.lu_cur = (g.lu_cur + 1) % 3; levelup_draw(); }
    if (pressed & KEY_UP)   { g.lu_cur = (g.lu_cur + 2) % 3; levelup_draw(); }
    if (pressed & KEY_A) {
        levelup_apply(g.lu_opt[g.lu_cur]);
        if (g.xp >= g.xpneed) { g.xp -= g.xpneed; g.lvl++; g.xpneed = xp_for(g.lvl); levelup_enter(); }
        else { g.state = ST_PLAY; hud_clear(); }
    }
}

/* ===================================================================== HUD */
static void hud_play(void) {
    hud_clear_rows(0, 2);
    hud_clear_rows(8, 9);
    put_text(0, 0, "HP", TP_GOLD);
    put_bar(0, 2, 10, g.hp * 80 / imax(1, g.maxhp), TP_HP);
    put_time(0, 13, g.t, TP_WHITE);
    put_text(0, 22, "LV", TP_GOLD); put_num(0, 25, g.lvl, 3, TP_WHITE);
    put_bar(1, 0, 30, g.xp * 240 / g.xpneed, TP_XP);
    if (g.boss.alive) {
        put_text(2, 0, "BOSS", TP_GOLD);
        put_bar(2, 5, 24, (int)(g.boss.hp * 192 / g.boss.maxhp), TP_HP);
    }
    if (g.warn > 0) {
        g.warn--;
        if (g.warn & 8) { put_text_c(8, "WARNING!", TP_GOLD); put_text_c(9, species[g.tier_species[3]].name, TP_WHITE); }
    }
    hud_clear_rows(19, 19);
    put_text(19, 0, "KO", TP_GOLD); put_num(19, 3, g.kills, 4, TP_WHITE);
}

/* ================================================================== states */
static void over_enter(int cleared) {
    g.state = ST_OVER; g.result = cleared;
    int t = g.t;
    g.new_best = 0;
    if (t > g.best) { g.best = t; g.new_best = 1; save_write(); }
    hud_clear();
    put_panel(5, 15, 3, 26);
    put_text_c(6, cleared ? "STAGE CLEAR!" : "FAINTED...", TP_GOLD);
    put_text(8, 6, "TIME", TP_GREY);  put_time(8, 16, t, TP_WHITE);
    put_text(9, 6, "LEVEL", TP_GREY); put_num(9, 16, g.lvl, 5, TP_WHITE);
    put_text(10, 6, "KO", TP_GREY);   put_num(10, 16, g.kills, 5, TP_WHITE);
    put_text(11, 6, "BEST", TP_GREY); put_time(11, 16, g.best, TP_WHITE);
    if (g.new_best) put_text(11, 22, "NEW!", TP_GOLD);
    put_text(13, 6, "A RETRY", TP_WHITE); put_text(13, 15, "B MENU", TP_WHITE);
}
static void play_update(void) {
    if (pressed & KEY_START) {
        g.state = ST_PAUSE; put_panel(8, 11, 9, 20); put_text_c(9, "PAUSED", TP_GOLD); put_text_c(10, "START: RESUME", TP_GREY);
        return;
    }
    g.t++;
    if (sfx_cd > 0) sfx_cd--;
    update_player();
    if (g.next_boss < NUM_BOSSES && !g.boss.alive && g.t >= BOSS_AT_SEC[g.next_boss] * 60) boss_start();
    update_spawner();
    update_enemies();
    update_boss();
    update_eshots();
    auto_fire();
    update_shots();
    update_pickups();
    cam_x = (g.px >> 8) - 120; cam_y = (g.py >> 8) - 88;
    if (g.hp <= 0) { g.hp = 0; over_enter(0); return; }
    if (g.win) { over_enter(1); return; }
    if (g.xp >= g.xpneed) { g.xp -= g.xpneed; g.lvl++; g.xpneed = xp_for(g.lvl); levelup_enter(); return; }
    hud_play();
}

#ifdef HOST
int game_main(void)
#else
int main(void)
#endif
{
#ifndef HOST
    REG_WAITCNT = 0x4317;          /* 3/1 ROM wait states + prefetch */
#endif
    gfx_init();
    sound_init();
    g.rng = 0x1234ABCDu; g.sel = 0;
    save_load();
    title_enter();
    for (;;) {
        input_poll();
        switch (g.state) {
            case ST_TITLE:
                title_update();
                if (pressed & (KEY_A | KEY_START)) run_start(g.sel);
                break;
            case ST_PLAY:    play_update(); break;
            case ST_LEVELUP: levelup_update(); break;
            case ST_PAUSE:
                if (pressed & KEY_START) { g.state = ST_PLAY; hud_clear(); }
                break;
            case ST_OVER:
                if (pressed & (KEY_A | KEY_START)) run_start(g.species);
                else if (pressed & KEY_B) title_enter();
                break;
        }
#ifdef DEBUG_PERF
        if (g.state == ST_PLAY) put_num(18, 26, REG_VCOUNT, 3, TP_GREY);   /* scanline when logic finished (>=160 = missed vblank) */
#endif
        oam_build();
        hw_wait_vblank();
        commit();
        hw_frame_done();
        g.frame++;
    }
}

#ifndef HOST
void hw_wait_vblank(void) { while (REG_VCOUNT >= 160) {} while (REG_VCOUNT < 160) {} }
void hw_frame_done(void) {}
#endif

/* host.c - PC test harness for the GBA game logic.
 *
 * It provides emulated VRAM / palette / OAM / registers (see gba.h, -DHOST) and
 * a small software PPU that renders them like the GBA would: mode 0 regular
 * tile backgrounds (4bpp, scroll, 32x32..64x64 maps) and 1D-mapped 4bpp OBJ
 * with sizes, flips and priorities.  It is NOT a general emulator - just enough
 * to check that what game.c writes to memory actually looks right.
 *
 *   host_sim --species N --frames F [--seed S] [--bot] [--shot f1,f2,...]
 *            [--out prefix] [--keys "frame:KEYS,..."]
 *   KEYS = letters from  A B S(tart) R L U D < >   (held from that frame on)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/game.h"

u16 hw_io[0x200];
u16 hw_pal_bg[256], hw_pal_obj[256];
u16 hw_vram[0xC000] __attribute__((aligned(4)));
u16 hw_oam[512] __attribute__((aligned(4)));
u8  hw_sram[0x10000];

/* ------------------------------------------------------------------ PPU */
static uint32_t fb[240 * 160];

static uint32_t col555(u16 c) {
    int r = c & 31, gg = (c >> 5) & 31, b = (c >> 10) & 31;
    return ((r << 3 | r >> 2) << 16) | ((gg << 3 | gg >> 2) << 8) | (b << 3 | b >> 2);
}
static int bg_pixel(int n, int x, int y, int *prio) {       /* returns palette colour index (0 = transparent) */
    u16 cnt = IO16(0x008 + n * 2);
    *prio = cnt & 3;
    int cb = ((cnt >> 2) & 3) * 0x4000, sb = ((cnt >> 8) & 31) * 0x800, size = cnt >> 14;
    int hofs = IO16(0x010 + n * 4) & 511, vofs = IO16(0x012 + n * 4) & 511;
    int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    int px = (x + hofs) & (w - 1), py = (y + vofs) & (h - 1);
    int tx = px >> 3, ty = py >> 3, blk = 0;
    if (size == 1) blk = tx >> 5; else if (size == 2) blk = ty >> 5; else if (size == 3) blk = (tx >> 5) + ((ty >> 5) << 1);
    u16 e = hw_vram[(sb + blk * 0x800) / 2 + (ty & 31) * 32 + (tx & 31)];
    int tile = e & 0x3FF, fx = px & 7, fy = py & 7;
    if (e & 0x400) fx = 7 - fx;
    if (e & 0x800) fy = 7 - fy;
    u8 byte = ((u8 *)hw_vram)[cb + tile * 32 + fy * 4 + (fx >> 1)];
    int ci = (fx & 1) ? byte >> 4 : byte & 15;
    return ci ? ((e >> 12) * 16 + ci) : 0;
}
static const int OBJ_W[3][4] = { {8,16,32,64}, {16,32,32,64}, {8,8,16,32} };
static const int OBJ_H[3][4] = { {8,16,32,64}, {8,8,16,32}, {16,32,32,64} };

static int obj_pixel(int x, int y, int *prio) {              /* palette index into OBJ pal, 0 = none */
    int best = 0, bp = 99;
    for (int i = 0; i < 128; i++) {
        u16 a0 = hw_oam[i * 4], a1 = hw_oam[i * 4 + 1], a2 = hw_oam[i * 4 + 2];
        if (a0 & 0x200) continue;                            /* hidden (affine not supported) */
        int shape = a0 >> 14, size = a1 >> 14;
        if (shape > 2) continue;
        int w = OBJ_W[shape][size], h = OBJ_H[shape][size];
        int oy = a0 & 255, ox = a1 & 511;
        int dy = (y - oy) & 255, dx = (x - ox) & 511;
        if (dy >= h || dx >= w) continue;
        if (a1 & 0x1000) dx = w - 1 - dx;
        if (a1 & 0x2000) dy = h - 1 - dy;
        int tile = (a2 & 0x3FF) + (dy >> 3) * (w >> 3) + (dx >> 3);
        u8 byte = ((u8 *)hw_vram)[0x10000 + tile * 32 + (dy & 7) * 4 + ((dx & 7) >> 1)];
        int ci = (dx & 1) ? byte >> 4 : byte & 15;
        if (!ci) continue;
        int p = (a2 >> 10) & 3;
        if (p < bp) { bp = p; best = (a2 >> 12) * 16 + ci; }   /* lowest index wins ties (loop order) */
    }
    *prio = bp;
    return best;
}
static void render(void) {
    u16 dc = hw_io[0];
    if (dc & DCNT_FORCE_BLANK) { for (int i = 0; i < 240 * 160; i++) fb[i] = 0xFFFFFF; return; }
    for (int y = 0; y < 160; y++) for (int x = 0; x < 240; x++) {
        uint32_t out = col555(hw_pal_bg[0]);
        int bgc[4] = {0}, bgp[4] = {9, 9, 9, 9};
        for (int n = 0; n < 4; n++) if (dc & (0x100 << n)) bgc[n] = bg_pixel(n, x, y, &bgp[n]);
        int op = 9, oc = 0;
        if (dc & DCNT_OBJ) oc = obj_pixel(x, y, &op);
        int done = 0;
        for (int p = 0; p < 4 && !done; p++) {
            if (oc && op == p) { out = col555(hw_pal_obj[oc]); done = 1; break; }
            for (int n = 0; n < 4; n++) if (bgc[n] && bgp[n] == p) { out = col555(hw_pal_bg[bgc[n]]); done = 1; break; }
        }
        fb[y * 240 + x] = out;
    }
}
static void save_ppm(const char *name) {
    FILE *f = fopen(name, "wb"); if (!f) return;
    fprintf(f, "P6\n240 160\n255\n");
    for (int i = 0; i < 240 * 160; i++) { fputc(fb[i] >> 16, f); fputc((fb[i] >> 8) & 255, f); fputc(fb[i] & 255, f); }
    fclose(f);
}

/* ------------------------------------------------------------ test driver */
static int opt_events = 0;
static int opt_frames = 600, opt_species = 0, opt_seed = 1, opt_bot = 0, opt_verbose = 0;
static const char *opt_out = "shot";
static int shots[64], nshots;
typedef struct { int frame; int keys; } KeyEv;
static KeyEv kev[1024]; static int nkev;
static int hf, cur_keys, bot_phase;
static int sel_presses_done, started;

static int parse_keys(const char *s) {
    int k = 0;
    for (; *s; s++) switch (*s) {
        case 'A': k |= KEY_A; break; case 'B': k |= KEY_B; break; case 'S': k |= KEY_START; break;
        case 'R': k |= KEY_R; break; case 'L': k |= KEY_L; break; case 'U': k |= KEY_UP; break;
        case 'D': k |= KEY_DOWN; break; case '<': k |= KEY_LEFT; break; case '>': k |= KEY_RIGHT; break;
    }
    return k;
}

/* flee-and-collect bot: scores 8 headings by distance to nearby enemies / gems */
static int bot_move_keys(void) {
    static const int dx8[8] = {1,1,0,-1,-1,-1,0,1}, dy8[8] = {0,1,1,1,0,-1,-1,-1};
    int best = -1; double bs = -1e18;
    /* safe? then walk straight to the nearest gem / heart */
    double dmin = 1e9, gmin = 1e9, gx = 0, gy = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) if (g.en[i].alive) {
        double ex = (g.en[i].x - g.px) / 256.0, ey = (g.en[i].y - g.py) / 256.0, dd = ex * ex + ey * ey;
        if (dd < dmin) dmin = dd;
    }
    for (int i = 0; i < MAX_GEMS; i++) if (g.gem[i].alive) {
        double ex = (g.gem[i].x - g.px) / 256.0, ey = (g.gem[i].y - g.py) / 256.0, dd = ex * ex + ey * ey;
        if (dd < gmin) { gmin = dd; gx = ex; gy = ey; }
    }
    if (dmin > 55.0 * 55.0 && gmin < 240.0 * 240.0) {
        int k = 0;
        if (gx > 6) k |= KEY_RIGHT; if (gx < -6) k |= KEY_LEFT;
        if (gy > 6) k |= KEY_DOWN;  if (gy < -6) k |= KEY_UP;
        if (k) return k;
    }
    for (int d = 0; d < 8; d++) {
        double sc = 0;
        double nx = (g.px >> 8) + dx8[d] * 28.0, ny = (g.py >> 8) + dy8[d] * 28.0;
        for (int i = 0; i < MAX_ENEMIES; i++) if (g.en[i].alive) {
            double ex = (g.en[i].x >> 8) - nx, ey = (g.en[i].y >> 8) - ny;
            double dd = ex * ex + ey * ey + 25;
            if (dd < 110 * 110) sc -= 4000.0 / dd * (g.en[i].tier == 3 ? 2 : 1);
        }
        for (int i = 0; i < MAX_GEMS; i++) if (g.gem[i].alive) {
            double ex = (g.gem[i].x >> 8) - nx, ey = (g.gem[i].y >> 8) - ny;
            double dd = ex * ex + ey * ey + 100;
            if (dd < 140 * 140) sc += 25.0 / dd * 10;
        }
        if (sc > bs) { bs = sc; best = d; }
    }
    int k = 0;
    if (dx8[best] > 0) k |= KEY_RIGHT; if (dx8[best] < 0) k |= KEY_LEFT;
    if (dy8[best] > 0) k |= KEY_DOWN;  if (dy8[best] < 0) k |= KEY_UP;
    return k;
}
static int bot_keys(void) {
    int k = 0;
    bot_phase++;
    if (g.state == ST_TITLE) {
        if (!started) {
            if (hf > 5 && sel_presses_done < opt_species) { if (hf & 1) { k |= KEY_RIGHT; sel_presses_done++; } }
            else if (sel_presses_done >= opt_species && hf > 8 + opt_species * 2 + opt_seed % 7) { k |= KEY_A; started = 1; }
        }
    } else if (g.state == ST_LEVELUP) {
        static const int prio[UP_COUNT] = { 2, 4, 5, 3, 0, 6, 7, 1 };   /* MULTI, POWER, RAPID, PIERCE, ... */
        int target = 0, bestp = 99;
        for (int i = 0; i < 3; i++) { int p = prio[g.lu_opt[i]]; if (p < bestp) { bestp = p; target = i; } }
        if (bot_phase & 1) { if (g.lu_cur != target) k |= KEY_DOWN; else k |= KEY_A; }
    } else if (g.state == ST_PLAY) {
        k |= bot_move_keys();
    } else if (g.state == ST_OVER) {
        /* leave it; the run is over */
    }
    return k;
}

static int max_oam;
static int ev_state = -1, ev_wait;
static void shot_now(const char *tag) {
    char name[256]; snprintf(name, sizeof name, "%s_%s.ppm", opt_out, tag); render(); save_ppm(name);
}
static void report_and_exit(void) {
    fprintf(stderr, "peak OAM entries used: %d / 128\n", max_oam);
    printf("species=%d (%s) seed=%d state=%d t=%d.%02ds level=%d kills=%d hp=%d/%d multi=%d pierce=%d result=%s\n",
           g.species, species[g.species].name, opt_seed, g.state, g.t / 60, (g.t % 60) * 100 / 60, g.lvl, g.kills,
           g.hp, g.maxhp, g.multi, g.pierce,
           g.state == ST_OVER ? (g.result ? "CLEAR" : "FAINTED") : "running");
    exit(0);
}

void hw_wait_vblank(void) {}
void hw_frame_done(void) {
    for (int i = 0; i < nshots; i++) if (shots[i] == hf) {
        char name[256]; snprintf(name, sizeof name, "%s_%05d.ppm", opt_out, hf);
        render(); save_ppm(name);
    }
    { int n = 0; for (int i = 0; i < 128; i++) if (!(hw_oam[i*4] & 0x200)) n++; if (n > max_oam) max_oam = n; }
    if (opt_events) {
        static int got_lu, got_over, got_pause; static int w_lu, w_over;
        if (g.state == ST_LEVELUP && !got_lu && ++w_lu > 4)  { shot_now("levelup"); got_lu = 1; }
        if (g.state == ST_OVER && !got_over && ++w_over > 20) { shot_now("over"); got_over = 1; }
        (void)got_pause;
    }
    hf++;
    int k = 0;
    for (int i = 0; i < nkev; i++) if (hf >= kev[i].frame) cur_keys = kev[i].keys;
    k = cur_keys;
    if (opt_bot) k |= bot_keys();
    hw_io[0x130 >> 1] = (u16)(~k & 0x3FF);
    if (opt_verbose && hf % 600 == 0 && g.state != ST_TITLE)
        fprintf(stderr, "  t=%ds hp=%d/%d lvl=%d xp=%d/%d kills=%d enemies=%d state=%d\n", g.t / 60, g.hp, g.maxhp, g.lvl, g.xp, g.xpneed, g.kills, g.nen, g.state);
    static int over_wait = 0;
    if (opt_bot && g.state == ST_OVER) over_wait++;
    if (hf >= opt_frames || over_wait > 2) report_and_exit();
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--species") && i + 1 < argc) opt_species = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) opt_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) opt_seed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bot")) opt_bot = 1;
        else if (!strcmp(argv[i], "-v")) opt_verbose = 1;
        else if (!strcmp(argv[i], "--events")) opt_events = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) opt_out = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            char *s = strdup(argv[++i]);
            for (char *t = strtok(s, ","); t && nshots < 64; t = strtok(NULL, ",")) shots[nshots++] = atoi(t);
        } else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            char *s = strdup(argv[++i]);
            for (char *t = strtok(s, ","); t && nkev < 1024; t = strtok(NULL, ",")) {
                char *c = strchr(t, ':'); if (!c) continue;
                kev[nkev].frame = atoi(t); kev[nkev].keys = parse_keys(c + 1); nkev++;
            }
        }
    }
    hw_io[0x130 >> 1] = 0x3FF;
    memset(hw_sram, 0xFF, sizeof hw_sram);
    return game_main();
}

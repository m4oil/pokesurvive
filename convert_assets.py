#!/usr/bin/env python3
"""
convert_assets.py - turns PokeSurvivor (web) data into GBA-ready C arrays.

Usage:
    python3 tools/convert_assets.py --src /path/to/pokesurvivor-main

Reads (from the web game's repo):
    assets/pokemon/<dex>/Walk.png    PMD sprite sheets (8 directions, N frames)
    js/data/sprites-meta.js          frame size / foot anchor per animation
    js/data/pokedex.js               name, types, hp/spd/atk, bst
    js/data/types.js                 type chart (softened x1.6 / x0.6 / x0.3)

Writes:
    source/assets_gen.c, source/assets_gen.h
    docs/species_contact_sheet.png   (visual check of what was converted)

GBA format notes
  * Every species uses ONE 16-colour OBJ palette (index 0 = transparent).
    PMD sprites are already <= 15 colours, so nothing is re-quantised.
  * Each frame is a 32x32 canvas = 16 8x8 4bpp tiles in row-major order
    (what OBJ "1D mapping" expects for a 32x32 sprite).
  * Only 5 of the 8 directions are stored; the other 3 are h-flips.
  * The foot anchor is moved to (16, 24) in every canvas, so all sprites
    share the same draw offset in the game.
"""
import argparse, json, math, os, random, re
from PIL import Image

# ---- the 36 species in the ROM: all fit 32x32 uncropped and have a 4-frame walk ----
SPECIES = [
    2, 4, 7, 9, 18, 25, 27, 31, 45, 46, 52, 54, 57, 58, 65, 66, 69, 74, 76, 79,
    94, 104, 120, 124, 135, 136, 152, 183, 197, 228, 241, 282, 304, 354, 361, 371,
]
TYPES = ["normal","fire","water","electric","grass","ice","fighting","poison","ground",
         "flying","psychic","bug","rock","ghost","dragon","dark","steel","fairy"]
TYPE_COL = {  # dark, mid, light (RGB 0-255) used for the projectile of each type
 "normal":((110,110,100),(190,190,170),(240,240,230)),"fire":((170,50,20),(240,110,40),(255,210,120)),
 "water":((30,70,190),(70,140,250),(170,220,255)),"electric":((170,130,10),(250,210,40),(255,250,160)),
 "grass":((30,110,40),(80,190,70),(190,250,150)),"ice":((50,130,170),(120,210,240),(215,250,255)),
 "fighting":((150,50,30),(210,90,60),(250,180,140)),"poison":((110,40,140),(170,80,200),(230,170,250)),
 "ground":((130,95,40),(200,160,80),(240,215,150)),"flying":((90,110,190),(150,170,240),(215,225,255)),
 "psychic":((180,40,100),(250,90,160),(255,180,220)),"bug":((90,120,20),(150,190,40),(215,245,130)),
 "rock":((110,95,50),(175,155,90),(225,210,150)),"ghost":((70,50,120),(120,90,180),(190,170,235)),
 "dragon":((60,50,170),(110,90,240),(190,170,255)),"dark":((50,40,50),(100,85,95),(165,150,165)),
 "steel":((100,110,120),(165,175,190),(220,230,240)),"fairy":((190,80,130),(245,140,190),(255,205,230)),
}
ANCHOR_X, ANCHOR_Y = 16, 24
NFRAMES, NDIRS = 4, 5

def rgb555(c):
    r, g, b = c[:3]
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)

def rgb888(c5):
    def e(v): return (v << 3) | (v >> 2)
    return (e(c5 & 31), e((c5 >> 5) & 31), e((c5 >> 10) & 31))

# --------------------------------------------------------------- parsing
def load_meta(src):
    txt = open(os.path.join(src, "js/data/sprites-meta.js"), encoding="utf8").read()
    meta = {}
    for m in re.finditer(r'^\s*(\d+): (\{.*\}),?\s*$', txt, re.M):
        meta[int(m.group(1))] = json.loads(m.group(2))
    return meta

def load_dex(src):
    txt = open(os.path.join(src, "js/data/pokedex.js"), encoding="utf8").read()
    dex = {}
    for m in re.finditer(r'^\s*(\{"dex":.*\}),?\s*$', txt, re.M):
        d = json.loads(m.group(1)); dex[d["dex"]] = d
    return dex

def load_chart(src):
    txt = open(os.path.join(src, "js/data/types.js"), encoding="utf8").read()
    body = txt[txt.index("const CHART"):txt.index("const SOFT")]
    soft = {2.0: 160, 0.5: 60, 0.0: 30}          # same softening as the web game
    chart = [[100]*18 for _ in range(18)]
    for m in re.finditer(r'^\s*(\w+):\s*\{(.*?)\}', body, re.M):
        a = m.group(1)
        if a not in TYPES: continue
        for k, v in re.findall(r'(\w+):\s*([0-9.]+)', m.group(2)):
            chart[TYPES.index(a)][TYPES.index(k)] = soft[float(v)]
    return chart

# ----------------------------------------------------------- sprite work
def species_frames(src, dex, meta):
    """returns (palette[16] of RGB555, frames[dir][frame] -> 32x32 list of indices)"""
    w = meta[dex]["a"]["Walk"]
    fw, fh, n = w["w"], w["h"], w["n"]
    assert n == NFRAMES, f"dex {dex}: walk has {n} frames"
    im = Image.open(os.path.join(src, f"assets/pokemon/{dex}/Walk.png")).convert("RGBA")
    ox = int(math.floor(ANCHOR_X - w["ax"] + 0.5)); oy = int(math.floor(ANCHOR_Y - w["ay"] + 0.5))
    cnt = {}
    for y in range(im.height):
        for x in range(im.width):
            p = im.getpixel((x, y))
            if p[3] >= 128: cnt[rgb555(p)] = cnt.get(rgb555(p), 0) + 1
    cols = [c for c, _ in sorted(cnt.items(), key=lambda kv: -kv[1])]
    assert len(cols) <= 15, f"dex {dex}: {len(cols)} colours"
    pal = [0] + cols + [0]*(15 - len(cols))
    idx = {c: i + 1 for i, c in enumerate(cols)}
    frames = []
    for d in range(NDIRS):
        row = []
        for f in range(NFRAMES):
            canvas = [0]*(32*32)
            for y in range(fh):
                for x in range(fw):
                    p = im.getpixel((f*fw + x, d*fh + y))
                    if p[3] < 128: continue
                    cx, cy = x + ox, y + oy
                    assert 0 <= cx < 32 and 0 <= cy < 32, f"dex {dex} dir {d} frame {f} leaves the canvas"
                    canvas[cy*32 + cx] = idx[rgb555(p)]
            row.append(canvas)
        frames.append(row)
    return pal, frames

def to_tiles(canvas, size):
    """size x size indices -> u32 words; 8x8 tiles row-major, 8 words per tile, pixel x = nibble x"""
    words = []
    for ty in range(size // 8):
        for tx in range(size // 8):
            for y in range(8):
                w = 0
                for x in range(8):
                    w |= (canvas[(ty*8 + y)*size + tx*8 + x] & 15) << (4*x)
                words.append(w)
    return words

# ------------------------------------------------------------- font / BG
FONT = {
 '0':".###.|#...#|#..##|#.#.#|##..#|#...#|.###.", '1':"..#..|.##..|..#..|..#..|..#..|..#..|.###.",
 '2':".###.|#...#|....#|...#.|..#..|.#...|#####", '3':".###.|#...#|....#|..##.|....#|#...#|.###.",
 '4':"...#.|..##.|.#.#.|#..#.|#####|...#.|...#.", '5':"#####|#....|####.|....#|....#|#...#|.###.",
 '6':".###.|#....|#....|####.|#...#|#...#|.###.", '7':"#####|....#|...#.|..#..|.#...|.#...|.#...",
 '8':".###.|#...#|#...#|.###.|#...#|#...#|.###.", '9':".###.|#...#|#...#|.####|....#|....#|.###.",
 'A':".###.|#...#|#...#|#####|#...#|#...#|#...#", 'B':"####.|#...#|#...#|####.|#...#|#...#|####.",
 'C':".###.|#...#|#....|#....|#....|#...#|.###.", 'D':"####.|#...#|#...#|#...#|#...#|#...#|####.",
 'E':"#####|#....|#....|####.|#....|#....|#####", 'F':"#####|#....|#....|####.|#....|#....|#....",
 'G':".###.|#...#|#....|#.###|#...#|#...#|.###.", 'H':"#...#|#...#|#...#|#####|#...#|#...#|#...#",
 'I':".###.|..#..|..#..|..#..|..#..|..#..|.###.", 'J':"..###|...#.|...#.|...#.|...#.|#..#.|.##..",
 'K':"#...#|#..#.|#.#..|##...|#.#..|#..#.|#...#", 'L':"#....|#....|#....|#....|#....|#....|#####",
 'M':"#...#|##.##|#.#.#|#.#.#|#...#|#...#|#...#", 'N':"#...#|##..#|#.#.#|#..##|#...#|#...#|#...#",
 'O':".###.|#...#|#...#|#...#|#...#|#...#|.###.", 'P':"####.|#...#|#...#|####.|#....|#....|#....",
 'Q':".###.|#...#|#...#|#...#|#.#.#|#..#.|.##.#", 'R':"####.|#...#|#...#|####.|#.#..|#..#.|#...#",
 'S':".####|#....|#....|.###.|....#|....#|####.", 'T':"#####|..#..|..#..|..#..|..#..|..#..|..#..",
 'U':"#...#|#...#|#...#|#...#|#...#|#...#|.###.", 'V':"#...#|#...#|#...#|#...#|#...#|.#.#.|..#..",
 'W':"#...#|#...#|#...#|#.#.#|#.#.#|##.##|#...#", 'X':"#...#|#...#|.#.#.|..#..|.#.#.|#...#|#...#",
 'Y':"#...#|#...#|.#.#.|..#..|..#..|..#..|..#..", 'Z':"#####|....#|...#.|..#..|.#...|#....|#####",
 ':':".....|..#..|..#..|.....|..#..|..#..|.....", '/':"....#|....#|...#.|..#..|.#...|#....|#....",
 '-':".....|.....|.....|#####|.....|.....|.....", '.':".....|.....|.....|.....|.....|.##..|.##..",
 '!':"..#..|..#..|..#..|..#..|..#..|.....|..#..", '+':".....|..#..|..#..|#####|..#..|..#..|.....",
 '%':"##..#|##..#|...#.|..#..|.#...|#..##|#..##", '>':"#....|.#...|..#..|...#.|..#..|.#...|#....",
 '<':"....#|...#.|..#..|.#...|..#..|...#.|....#", '*':"..#..|#.#.#|.###.|#####|.###.|#.#.#|..#..",
}
FONT_ORDER = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:/-.!+%><*"   # tile index = position
T_PANEL = len(FONT_ORDER)          # solid dark tile
T_BAR = T_PANEL + 1                # 9 bar tiles (0..8 px filled)
T_HEART = T_BAR + 9                # small heart icon for the HUD
T_COUNT = T_HEART + 1
T_PGLYPH = T_COUNT                 # second glyph set with a solid panel-colour background (index 3)
BG_TOTAL = T_COUNT + len(FONT_ORDER)

def font_tile(ch, bg=0):
    px = [[bg]*8 for _ in range(8)]
    if ch != ' ':
        for y, r in enumerate(FONT[ch].split('|')):
            for x, c in enumerate(r):
                if c == '#': px[y][x] = 1
        for y in range(7):                      # 1px drop shadow (index 2), down-right
            for x in range(7):
                if px[y][x] == 1 and px[y+1][x+1] == bg: px[y+1][x+1] = 2
    return px

def tile_words(px):
    return [sum((px[y][x] & 15) << (4*x) for x in range(8)) for y in range(8)]

def bar_tile(k):
    px = [[0]*8 for _ in range(8)]
    for x in range(8):
        px[1][x] = 3; px[6][x] = 3
        for y in range(2, 6): px[y][x] = 1 if x < k else 2
    return px

def heart_tile():
    rows = ["........", ".##..##.", "#**##**#", "#******#", "#******#", ".#****#.", "..#**#..", "...##..."]
    return [[{'.':0,'#':3,'*':1}[c] for c in r] for r in rows]

def floor_tiles():
    """pixel indices: 1 base, 2 light, 3 dark, 4 white, 5 yellow, 6 flower centre, 7 pink, 8/9 pebbles"""
    def base(seed, light, dark):
        r = random.Random(seed)
        px = [[1]*8 for _ in range(8)]
        for _ in range(light): px[r.randrange(8)][r.randrange(8)] = 2
        for _ in range(dark):  px[r.randrange(8)][r.randrange(8)] = 3
        return px
    t = [base(1, 4, 3), base(2, 3, 5)]
    tuft = base(3, 1, 1)
    for (x, y, c) in [(3,5,3),(3,4,3),(2,3,3),(4,3,3),(3,3,2),(5,6,3),(5,5,3),(6,4,3),(4,6,2)]: tuft[y][x] = c
    t.append(tuft)
    for col, seed in ((4, 10), (5, 11), (7, 12)):          # white, yellow, pink flowers
        f = base(seed, 2, 2)
        for (x, y) in [(3,2),(2,3),(4,3),(3,4)]: f[y][x] = col
        f[3][3] = 6
        f[6][6] = 3; f[6][5] = 3
        t.append(f)
    peb = base(20, 2, 2)
    for (x, y) in [(2,2),(3,2),(2,3),(3,3),(5,5),(6,5),(5,6)]: peb[y][x] = 8
    for (x, y) in [(2,4),(3,4),(6,6)]: peb[y][x] = 9
    t.append(peb)
    return t   # 7 tiles: 0,1 grass; 2 tuft; 3 white; 4 yellow; 5 pink; 6 pebbles

# ---- OBJ misc (palette bank 0 = shots, bank 6 = gems/heart/shadow) ----
def orb(r):
    c = [0]*(16*16)
    cx = cy = 7.5
    for y in range(16):
        for x in range(16):
            d = math.hypot(x - cx, y - cy)
            if d <= r: c[y*16+x] = 2
            if r - 1.2 < d <= r: c[y*16+x] = 1
            if d <= r - 1.2 and math.hypot(x - cx + 1.8, y - cy + 1.8) <= r*0.38: c[y*16+x] = 3
    return c

GEM = ["........", "...11...", "..1221..", ".123321.", ".122221.", "..1221..", "...11...", "........"]
def gem_tile(base):  # palette indices base..base+2 = dark, mid, light
    mp = {'.': 0, '1': base, '2': base + 1, '3': base + 2}
    return [[mp[c] for c in r] for r in GEM]
def heart_obj():
    rows = ["........", ".AA..AA.", "ABBAABBA", "ABCBBBBA", "ABBBBBBA", ".ABBBBA.", "..ABBA..", "...AA..."]
    mp = {'.':0, 'A':7, 'B':8, 'C':9}
    return [[mp[c] for c in r] for r in rows]
def shadow():
    c = [[0]*16 for _ in range(8)]
    for y in range(8):
        for x in range(16):
            if ((x-7.5)/7.5)**2 + ((y-3.5)/3.5)**2 <= 1: c[y][x] = 13
    return c

# ---------------------------------------------------------------- output
def arr(name, words, ctype="const u32", per=8):
    out = [f"{ctype} {name}[{len(words)}] = {{"]
    for i in range(0, len(words), per):
        out.append("  " + ",".join(f"0x{w:08X}" for w in words[i:i+per]) + ",")
    out.append("};\n")
    return "\n".join(out)

def pal_arr(name, p):
    return f"const u16 {name}[256] = {{\n" + "\n".join("  " + ",".join(f"0x{v:04X}" for v in p[i:i+16]) + "," for i in range(0, 256, 16)) + "\n};\n"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="path to the pokesurvivor-main folder")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    a = ap.parse_args()
    meta, dex, chart = load_meta(a.src), load_dex(a.src), load_chart(a.src)

    sp_rows, pals, tiles, sheet_imgs = [], [], [], []
    for k in SPECIES:
        d = dex[k]
        pal, frames = species_frames(a.src, k, meta)
        name = re.sub(r"[^A-Z0-9]", "", d["name"].upper())[:10]
        t = [TYPES.index(x) for x in d["types"]]
        assert d["hp"] < 256 and d["spd"] < 256
        sp_rows.append((name, t[0], t[1] if len(t) > 1 else 255, d["hp"], d["spd"], round(d["atk"]*100), d["bst"], k))
        pals.append(pal)
        for dr in range(NDIRS):
            for f in range(NFRAMES):
                tiles.extend(to_tiles(frames[dr][f], 32))
        sheet_imgs.append((pal, frames))
        print(f"  #{k:<4}{name:<11} types={d['types']} hp={d['hp']} spd={d['spd']} atk={d['atk']} bst={d['bst']}")

    order = sorted(range(len(SPECIES)), key=lambda i: (sp_rows[i][6], sp_rows[i][7]))

    # ---------------- header
    h = ["/* GENERATED by tools/convert_assets.py - do not edit by hand. */",
         "#ifndef ASSETS_GEN_H\n#define ASSETS_GEN_H\n#include \"gba.h\"\n",
         f"#define NUM_SPECIES {len(SPECIES)}\n#define NUM_TYPES 18\n#define NUM_DIRS {NDIRS}\n#define NUM_FRAMES {NFRAMES}",
         "typedef struct { char name[11]; u8 t1, t2; u8 hp, spd; u8 atk; u16 bst; u16 dex; } Species;",
         "extern const Species species[NUM_SPECIES];",
         "extern const u8 species_by_bst[NUM_SPECIES];   /* species indices, weakest first */",
         "extern const u16 species_pal[NUM_SPECIES][16];",
         "/* 32x32 4bpp frame = 128 words; [species][dir][frame][word] */",
         "extern const u32 (*const species_tiles)[NUM_DIRS][NUM_FRAMES][128];",
         "extern const char* const type_name[NUM_TYPES];",
         "extern const u16 type_color[NUM_TYPES][3];",
         "extern const u8 type_chart[NUM_TYPES][NUM_TYPES];   /* percent: attacker row x defender column */",
         f"#define FONT_ORDER \"{FONT_ORDER}\"",
         f"#define T_PANEL {T_PANEL}\n#define T_BAR {T_BAR}\n#define T_HEART {T_HEART}\n#define T_PGLYPH {T_PGLYPH}\n#define BG_FONT_TILES {BG_TOTAL}",
         "extern const u32 font_tiles[BG_FONT_TILES * 8];",
         "#define FLOOR_TILES 7",
         "extern const u32 floor_tiles[FLOOR_TILES * 8];",
         "extern const u16 bg_pal[256];",
         "extern const u16 obj_misc_pal[256];   /* bank 0 = shot (rewritten per run), 6 = gems/heart/shadow, 7 = white flash */",
         "#define MISC_SHOT_A 0     /* 16x16 = 4 tiles */\n#define MISC_SHOT_B 4\n#define MISC_GEM_S 8\n#define MISC_GEM_M 9\n#define MISC_GEM_L 10\n#define MISC_HEART 11\n#define MISC_SHADOW 12   /* 16x8 = 2 tiles */\n#define MISC_TILES 14",
         "extern const u32 misc_tiles[MISC_TILES * 8];",
         "#endif\n"]
    open(os.path.join(a.out, "source/assets_gen.h"), "w").write("\n".join(h))

    # ---------------- source
    c = ['/* GENERATED by tools/convert_assets.py - do not edit by hand.',
         ' * Sprites: SpriteCollab (https://github.com/PMDCollab/SpriteCollab), CC BY-NC 4.0 - see CREDITS.md.',
         ' * Pokemon (c) Nintendo / Creatures / GAME FREAK. Non-commercial fan project. */',
         '#include "assets_gen.h"\n']
    c.append("const Species species[NUM_SPECIES] = {")
    for r in sp_rows: c.append(f'  {{"{r[0]}",{r[1]},{r[2]},{r[3]},{r[4]},{r[5]},{r[6]},{r[7]}}},')
    c.append("};\n")
    c.append("const u8 species_by_bst[NUM_SPECIES] = {" + ",".join(map(str, order)) + "};\n")
    c.append("const char* const type_name[NUM_TYPES] = {" + ",".join(f'"{t.upper()}"' for t in TYPES) + "};\n")
    c.append("const u16 type_color[NUM_TYPES][3] = {")
    for t in TYPES: c.append("  {" + ",".join(f"0x{rgb555(x):04X}" for x in TYPE_COL[t]) + "},")
    c.append("};\n")
    c.append("const u8 type_chart[NUM_TYPES][NUM_TYPES] = {")
    for row in chart: c.append("  {" + ",".join(str(v) for v in row) + "},")
    c.append("};\n")
    c.append("const u16 species_pal[NUM_SPECIES][16] = {")
    for p in pals: c.append("  {" + ",".join(f"0x{v:04X}" for v in p) + "},")
    c.append("};\n")
    c.append(arr("species_tiles_flat", tiles))
    c.append("const u32 (*const species_tiles)[NUM_DIRS][NUM_FRAMES][128] =\n  (const u32 (*)[NUM_DIRS][NUM_FRAMES][128])species_tiles_flat;\n")

    ft = []
    for ch in FONT_ORDER: ft.extend(tile_words(font_tile(ch)))
    ft.extend(tile_words([[2]*8 for _ in range(8)]))                 # panel
    for k in range(9): ft.extend(tile_words(bar_tile(k)))
    ft.extend(tile_words(heart_tile()))
    for ch in FONT_ORDER: ft.extend(tile_words(font_tile(ch, bg=3)))
    c.append(arr("font_tiles", ft))
    fl = []
    for t in floor_tiles(): fl.extend(tile_words(t))
    c.append(arr("floor_tiles", fl))

    # BG palette: bank0 floor, 1 white text, 2 hp bar, 3 xp bar, 4 gold text, 5 grey text
    bp = [0]*256
    bp[0] = rgb555((60, 128, 56))
    for i, col in {1:(88,168,72), 2:(112,192,88), 3:(64,136,64), 4:(240,240,224), 5:(248,216,72),
                   6:(216,120,40), 7:(240,120,160), 8:(150,150,140), 9:(110,110,100)}.items():
        bp[i] = rgb555(col)
    DARK = (24, 20, 48)
    for n, cols in {1:{1:(255,255,255),2:DARK,3:DARK}, 2:{1:(232,56,72),2:(70,24,40),3:(255,255,255)},
                    3:{1:(72,184,255),2:(24,40,80),3:(255,255,255)}, 4:{1:(255,224,72),2:DARK,3:DARK},
                    5:{1:(205,205,225),2:DARK,3:DARK}}.items():
        for i, col in cols.items(): bp[n*16 + i] = rgb555(col)
    c.append(pal_arr("bg_pal", bp))

    op = [0]*256
    for i, col in enumerate([(110,110,100),(190,190,170),(240,240,230)], start=1): op[i] = rgb555(col)
    g = {1:(40,80,200),2:(80,140,255),3:(200,230,255),4:(30,120,40),5:(70,200,90),6:(200,255,200),
         7:(170,30,50),8:(240,60,80),9:(255,170,180),10:(170,120,20),11:(250,200,40),12:(255,245,170),13:(40,100,44)}
    for i, col in g.items(): op[6*16 + i] = rgb555(col)
    for i in range(1, 16): op[7*16 + i] = 0x7FFF
    c.append(pal_arr("obj_misc_pal", op))

    ms = []
    ms.extend(to_tiles(orb(6.6), 16)); ms.extend(to_tiles(orb(5.2), 16))
    for base in (1, 4, 10): ms.extend(tile_words(gem_tile(base)))
    ms.extend(tile_words(heart_obj()))
    sh = shadow()
    for tx in range(2): ms.extend(tile_words([row[tx*8:tx*8+8] for row in sh]))
    c.append(arr("misc_tiles", ms))
    open(os.path.join(a.out, "source/assets_gen.c"), "w").write("\n".join(c))

    # ---------------- contact sheet for visual verification
    cols = 9; rows_n = math.ceil(len(SPECIES)/cols); S = 3
    cw = (32*3 + 6)*S; chh = (32 + 14)*S
    sheet = Image.new("RGB", (cols*cw, rows_n*chh), (88, 168, 72))
    for i, (pal, frames) in enumerate(sheet_imgs):
        ox, oy = (i % cols)*cw, (i // cols)*chh
        for j, (dr, fr) in enumerate([(0, 1), (2, 1), (4, 1)]):
            im = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
            for p, v in enumerate(frames[dr][fr]):
                if v: im.putpixel((p % 32, p // 32), rgb888(pal[v]) + (255,))
            im = im.resize((32*S, 32*S), Image.NEAREST)
            sheet.paste(im, (ox + j*(32*S + 2*S), oy + 7*S), im)
    sheet.save(os.path.join(a.out, "docs/species_contact_sheet.png"))
    print(f"OK: {len(SPECIES)} species, {len(tiles)*4//1024} KB of sprite tiles")

if __name__ == "__main__":
    main()

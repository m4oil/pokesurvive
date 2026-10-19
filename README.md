# PokeSurvivor GBA

A Game Boy Advance port/reimagining of the browser game *PokéSurvivor*: pick one of **36 Pokémon**,
auto-fire at swarms, collect gems, level up, survive **5:00**.

![title](docs/screenshot_title.png) ![play](docs/screenshot_play.png)
![level up](docs/screenshot_levelup.png)

## Get the ROM (pick one)
The zip is **source code**. A GBA ROM is a `.gba` file that has to be compiled from it.

**A. No install - build in the cloud (GitHub, free)**
1. Create a new GitHub repository and upload everything from this folder (including the hidden `.github` folder).
2. Open the **Actions** tab -> *Build GBA ROM* -> wait ~1 minute -> download the **pokesurvivor-gba-rom** artifact.
3. Unzip it: `pokesurvivor.gba` is the ROM.

**B. Docker one-liner** (in this folder): `docker run --rm -v "$PWD":/work -w /work devkitpro/devkitarm make`
-> `build/pokesurvivor.gba`

**C. Local devkitPro install** - see "Build the ROM" below.

## Controls
| | |
|---|---|
| D-pad | move (the Pokémon fires automatically at the nearest foe) |
| L / R / ◀ ▶ (title) | change Pokémon |
| A / START (title) | start |
| START (in game) | pause |
| UP/DOWN + A | choose a level-up upgrade |
| A / B (game over) | retry / back to menu |

Best time is saved to cartridge SRAM.

## Build the ROM
1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the **GBA** package (devkitARM).
2. `make` → `build/pokesurvivor.gba`. Run it in mGBA / VBA-M / on hardware (flashcart).
   `make perf` adds a scanline readout at the right of the bottom HUD row; a value ≥ 160 means
   the logic overran VBlank (a dropped frame).

The sprite/data files (`source/assets_gen.*`) are already generated. To regenerate (e.g. to change the
roster in `tools/convert_assets.py`): `make assets SRC=/path/to/pokesurvivor-main` (needs Python 3 + Pillow).

## What was and wasn't ported
Kept: Pokémon stats (HP/speed/attack), the **softened type chart** (x1.6 / x0.6 / x0.3, both ways), level-ups,
XP gems, hearts, elite bosses, tiered enemy waves, survival timer, SRAM best time.
Cut (not feasible on GBA): 966 Pokémon, shinies/gacha, weather, rifts, accounts/cloud saves, co-op, rankings,
Web Audio music. Sound is basic PSG blips.

## How it fits the hardware
* 36 species × 5 directions × 4 walk frames, **32x32, one 16-colour palette each** (PMD sprites already
  fit - nothing is re-quantised), 360 KB of ROM. The other 3 directions are h-flips.
* A run keeps the player frame streamed into VRAM and 4 enemy species resident (3 directions × 4 frames each),
  one per tier, picked by base-stat total so weak → strong. Enemy cap is 30 on screen.
* BG0 is an endless floor: a 64x64 ring buffer whose edge rows/columns are rewritten from a hash as you move.
* All VRAM/OAM writes happen right after VBlank starts, with 16/32-bit accesses only.

## Testing without a console (what was verified here)
`make host` builds the same `game.c` against emulated memory plus a small software PPU (`host/host.c`).
```
build/host_sim --species 14 --bot --frames 19000          # bot plays a run, prints the result
build/host_sim --species 5 --frames 600 --shot 300 --out build/s && python3 host/ppm2png.py build/s_00300.ppm
```
The harness rendered the screenshots in `docs/`, ran bot playthroughs for several species, and showed a
peak of ~43 of 128 OAM entries. **It cannot measure ARM7 CPU time**, and the ARM build itself was not compiled
in the environment this was written in (no devkitARM there). Use `make perf` on first run.

## Tuning
Constants are at the top of `source/game.c` and in `update_spawner`, `update_enemies`, `enemy_hp_for`, `xp_for`.
A flee-and-collect bot clears 5:00 easily; an idle player dies in ~25 s. Human difficulty is untuned.

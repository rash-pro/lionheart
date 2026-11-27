# Remaster-style Enhancements, HD Font, HD Textures, and HD Models

All enhancements in Lionheart are **off by default** and configurable from **Menu → Enhancements**, `lionheart.ini`, or the command line.

Lionheart keeps a strict separation between **gameplay boosters** and **presentation-only visual upgrades**:

- **Presentation upgrades** (`hd-font`, `hd-textures`, `hd-models`, CRT, PGXP, smooth motion) run strictly in the Vulkan presentation layer. They never alter guest RAM, software reference output, or RetroAchievements.
- **Gameplay boosters** (`no-encounters`, `battle-assist`) modify guest RAM and **permanently disable RetroAchievements** for the current session (and any save states created afterward).

---

## 1. Boosters & Speed

| Option (`lionheart.ini` / CLI) | Values (default first) | Hotkey | RetroAchievements |
| --- | --- | --- | --- |
| `speed-boost` / `--speed-boost` | `0`, `1` | `T` / `Guide + L2` | Allowed (casual pacing) |
| `no-encounters` / `--no-encounters` | `0`, `1` | `Y` / `Guide + R3` | **Permanently disables RA** |
| `battle-assist` / `--battle-assist` | `0`, `1` | `U` / `Guide + L3` | **Permanently disables RA** |

Whenever one or more boosters are active (and `--osd 1` is enabled), Lionheart shows compact Remaster-style status badges (`3x`, `NO ENC`, `ASSIST`) in the bottom-left corner of the visible game area (hidden in the pause menu and excluded from screenshots). Pressing the hotkey for **No random battles** or **Battle assist** while RetroAchievements is active opens the confirmation dialog first instead of silently disabling achievements.

### RetroAchievements lockout behavior
When you turn on **No random battles** or **Battle assist**:
1. If RetroAchievements is active or signed in, Lionheart displays a confirmation prompt first.
2. Confirming signs you out, removes your stored `ra/credentials` token, hides the Achievements pane, and permanently blocks re-enabling RetroAchievements for the rest of the session (even if you later toggle the booster off).
3. Save states created after enabling a gameplay booster record a taint bit (`SAVESTATE_MODE_BOOSTERS_TAINTED`) so loading them also locks out RetroAchievements.

---

## 2. HD Font (Vulkan)

| Option (`lionheart.ini` / CLI) | Values (default first) | Description |
| --- | --- | --- |
| `hd-font` / `--hd-font` | `off`, `builtin`, `custom` | Replaces FF8's 4bpp `sysfnt` glyph sheet in Vulkan |
| `hd-font-path` / `--hd-font-path` | `PATH` | Custom `.ttf`/`.otf` font or `.png` atlas when `hd-font=custom` |

- **`builtin`** renders the bundled open-source **Barlow** (OFL-1.1) font into an HD glyph atlas (2×, 4× or 8×, following `internal-scale`) aligned to FF8's 21-column `12×12` cell grid, tinted live from each PS1 CLUT palette (preserving white, yellow, grey, red, blue text and shadows).
- The game, not the texture, decides how far the pen moves after each character, and its font is nearly fixed-width (`n`, `s`, `w` share one advance; `t` and `r` get almost as much). Each HD glyph is therefore fitted to its own slot, read from the live sheet (a glyph's advance is its stroke width plus two texels): the glyph is drawn wider or narrower as the slot asks, with its vertical strokes corrected back to the face's weight, so words are evenly spaced and every letter keeps the same stroke. Capitals are as tall as the game's (nine texels), `I` gets the serifs the game's `I` has, and the `Ⅷ`/`Ⅵ`/`Ⅱ` cells are drawn as barred Roman numerals.
- The same font replaces the text cells of the menu/battle icon sheet (`icon.tex`): menu, battle and Triple Triad digits, `/ : %`, window header badges (`HELP`, `STATUS`, `COMMAND`, …), `PLAY`/`SeeD`/`Lv`/`HP`/`TIME`, the `Str`…`Luck` stat labels and `Miss`. Graphics on that sheet (cursor, window gradient, status/element icons, button art, the stylised `Trigger!`/`Perfect!!` banners) stay native.
- Battle party names are drawn in the HD font too. The game renders each name on the CPU into a strip of the icon sheet, in a bold face that exists in no texture, so the strip is identified against the names the game can show (the two the player can rename from the save data, the rest from the game's own text; USA `SLUS-00892` layout, read-only). A strip is replaced only when exactly one of those names accounts for its ink glyph by glyph against the live `sysfnt` sheet; otherwise it stays as the game drew it.
- Both sheets are recognised by their structure wherever they sit in VRAM, not by the shape of the upload that carried them, so the HD font does not depend on how a battle load, a save-state load or a mid-game settings change delivers them.
- **`custom`** lets you select any `.ttf`/`.otf` font on your PC or a custom PNG font atlas.

---

## 3. HD Textures (Vulkan)

| Option (`lionheart.ini` / CLI) | Values (default first) | Description |
| --- | --- | --- |
| `hd-textures` / `--hd-textures` | `0`, `1` | Enables VRAM texture replacement from `textures-dir` |
| `textures-dir` / `--textures-dir` | `PATH` | Folder containing replacement `.png` textures |
| `dump-textures` / `--dump-textures` | `PATH` | Optional modder helper: dumps seen VRAM textures as PNGs with their hashes |

Replacement files in `textures-dir` are matched by 64-bit FNV-1a hash:
- `<tex_hash>_<clut_hash>.png` (exact texture + palette match)
- `<tex_hash>.png` (fallback for any palette)

Replacement dimensions must be an integer power-of-two multiple (`2×`, `4×`, `8×`, etc.) of the original local PS1 texture size. PNGs decode asynchronously on a background worker thread so gameplay never stutters.

---

## 4. HD Models (Vulkan — Live 3D Rendering, Pack Format & Importer)

| Option (`lionheart.ini` / CLI) | Values (default first) | Description |
| --- | --- | --- |
| `hd-models` / `--hd-models` | `0`, `1` | Enables live Vulkan 3D HD model replacement from `models-dir` (real-time toggleable) |
| `models-dir` / `--models-dir` | `PATH` | Folder containing a Lionheart glTF 2.0 (`.glb`) model pack |

Lionheart ships **zero Square Enix assets**. HD 3D models are **strictly presentation-only**: they render directly into Vulkan's scaled VRAM plane (`1×`–`8×`, MSAA `1×`–`8×`, CRT-compatible) and **never** modify guest RAM, CPU state, save states, RetroAchievements, or the bit-exact native software reference rasterizer.

### How live 3D replacement & compositing work
1. **Zero overhead when off or empty:** When `hd-models=0` or no `.glb` models are found in `models-dir`, no GTE projection hook or scene observer runs.
2. **Live GTE bone extraction & lighting capture:** On every `RTPS`/`RTPT` projection, Lionheart captures consecutive bone transform runs (`rotation[5]`, `translation[3]`, `OFX`, `OFY`, `H`, and depth `SZ`) and clusters them into active 3D skeletons, coexisting cleanly with both **PGXP** (`off`/`vertex`/`perspective`) and **Smooth Motion** (`0`/`1`). Average PS1 vertex modulation color (`128` = neutral `1.0×`) is captured from the replaced model's polygons so HD meshes match field shadows, battle lighting, damage flashes, and screen fades.
3. **Selective PS1 suppression & Ordering-Table depth/mask compositing:** Replaced PS1 character triangles are suppressed only in the Vulkan renderer (never in the software reference rasterizer). Non-replaced NPCs, enemies, backgrounds, effects, and UI continue rendering normally. Whenever a foreground pre-rendered field mask (columns, doorways, railings), overlapping effect, or UI/HUD primitive arrives over a replaced model's screen bounds, Lionheart flushes the HD 3D render pass first—with hardware Z-buffering (`D32_SFLOAT` / `D24`) and opaque / alpha-masked (`MASK`) / alpha-blended (`BLEND`) material passes—so characters walk cleanly behind foreground field masks and behind dialogue/HUD overlays.
4. **Real-time toggle:** Toggling **HD models** on or off in **Menu → Enhancements** takes effect immediately on the next frame without restarting.

### `manifest.json` schema
Drop `.glb` files directly under `models-dir` (`field/d000.glb`, `battle/c0m001.glb`, `world/squall.glb`) for automatic convention discovery, or add a `manifest.json` for full control:

```json
{
  "models": {
    "field/d000": {
      "file": "field/d000.glb",
      "scale": 1.0,
      "offset": [0.0, 0.0, 0.0],
      "rotation_euler": [0.0, 0.0, 0.0],
      "flip_winding": false,
      "double_sided": false,
      "bone_remap": [0, 1],
      "vram_texture_hashes": ["0123456789abcdef"],
      "gte_run_keys": []
    },
    "battle/c0m001": {
      "file": "battle/c0m001.glb",
      "scale": 1.0
    }
  }
}
```

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `file` | string | *(required)* | Relative path to the glTF 2.0 binary `.glb` file (supports embedded PNG/JPEG textures, `POSITION`, `NORMAL`, `TEXCOORD_0`, `JOINTS_0`, `WEIGHTS_0`, `OPAQUE`/`MASK`/`BLEND` materials) |
| `scale` | float | `1.0` | Uniform model-space scale applied before bone skinning |
| `offset` | `[x, y, z]` | `[0, 0, 0]` | Model-space translation offset applied before bone skinning |
| `rotation_euler` | `[rx, ry, rz]` | `[0, 0, 0]` | Model-space Euler rotation in **degrees** (`Rz * Ry * Rx`) |
| `flip_winding` | bool | `false` | Swaps triangle front-face winding (`CW` vs `CCW`) |
| `double_sided` | bool | `false` | Disables backface culling for all primitives in the model |
| `bone_remap` | `int[]` | `[]` | Maps glTF joint index `i` to PS1 GTE bone run index `bone_remap[i]` |
| `vram_texture_hashes` | `string[]` | `[]` | 64-bit hex FNV-1a VRAM texture hashes (same format as `--dump-textures`) for hash-exact matching in Field, Battle, or World Map |
| `gte_run_keys` | `string[]` | `[]` | Optional 64-bit hex GTE transform-run keys for exact geometry-signature matching |

### Automatic FF8 PC / FFNx mod importer (native C++)
Lionheart includes a built-in native C++ mod importer with zero external dependencies (CPU BC1/BC2/BC3/BC7 DDS decoder, IROS `.iroj`/`.iro` archive reader, Junction VIII / 7th Heaven `mod.xml` evaluator, PS1 disc indexer, and `.mch` → glTF 2.0 `.glb` converter):

- **From the in-app GUI:** Open **Menu → Enhancements**. Use **Import .iro** for a single `.iro` or `.iroj` archive, or **Import folder** for a library of archives or an unpacked mod. If the pack has supported `mod.xml` settings, choose them before importing. The importer keeps existing files in the destination folders and reports skipped files in the import log.
- **Manage imported packs:** Find the pack under **Installed mods** to view its log, enable or disable it, or remove it. Keep the original output folders selected when managing a pack. To reimport a pack with different options, remove the current pack first.
- **From the CLI:**
  ```sh
  lionheart --disc /path/to/FF8-Disc1.cue --import-mods /path/to/ff8-mods/library
  ```
  You can repeat `--import-mods` for multiple archives or folders and override output paths with `--textures-dir`, `--models-dir`, and `--movies`.

### Testing & local pack importer (`tools/hd_models_import.py`)
Lionheart includes a clean-room MIT Python 3 CLI helper (`tools/hd_models_import.py`, zero external dependencies):

1. **Generate and validate a synthetic test `.glb` pack** (for end-to-end verification):
   ```sh
   python3 tools/hd_models_import.py --make-test-pack ~/.local/share/lionheart/models-test
   python3 tools/hd_models_import.py --validate-pack ~/.local/share/lionheart/models-test
   ```
   Then launch Lionheart with `--hd-models 1 --models-dir ~/.local/share/lionheart/models-test` (or pick the folder in **Menu → Enhancements**).
2. **Inspect or extract your own legally-owned Steam *Final Fantasy VIII Remastered* `main.zzz` archive:**
   ```sh
   python3 tools/hd_models_import.py --list-zzz "/path/to/FF8 Remastered/main.zzz" --filter "d000|c0m001"
   python3 tools/hd_models_import.py --extract-zzz "/path/to/FF8 Remastered/main.zzz" --out ~/.local/share/lionheart/remaster-raw --filter "d000|c0m001"
   ```
   Converted `.glb` files placed in `~/.local/share/lionheart/models/` are for personal local use only—never redistribute copyrighted assets.
3. **Community FF8 modding hubs:**
   - **Tsunamods / Junction VIII** — `https://www.tsunamods.com/`
   - **Qhimm Forums** — `https://forums.qhimm.com/`
   - **Nexus Mods (FF8 / FF8 Remastered)** — `https://www.nexusmods.com/finalfantasy8remastered`

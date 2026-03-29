# GPU Rendering Corruption — Investigation Summary

**Branch:** `phase3/gpu-visual-fixes`
**Date:** 2026-03-29
**Status:** Partially fixed, **root cause identified: atlas pixel alpha=0 for opaque sprites**

---

## Symptom Overview

The GPU rendering path (wgpu) shows visual corruption on the main menu and in-game:
1. **Missing ground tiles** — large dark navy gaps where terrain should be
2. **Floating buildings** — parent sprites (buildings, vehicles) render correctly but without ground beneath them
3. **Zoom-dependent worsening** — corruption gets worse at far zoom levels
4. **State pollution** — errors persist across frames after zoom/scroll operations
5. **Atlas metadata mismatch logs** — sprite 2771 decoded with different dimensions on re-decode

## Architecture Overview

```
Sprite Decode (spritecache.cpp)
  → ResizeSprites() populates all zoom levels from Root (ZoomLevel::Min = In4x)
  → Atlas Upload: sprite.Root() RGBA pixels → SpriteAtlas::Upload(id, data, ZoomLevel::Min)
  → AtlasEntry stored: {page, u0/v0/u1/v1, width, height, x_offs, y_offs}

Viewport Drawing (viewport.cpp)
  → ViewportAddLandscape() collects visible tile sprites
  → ViewportAddVehicles() collects visible parent sprites
  → ViewportDrawTileSprites() → EmitGpuSpriteCommand() per tile
  → _vp_sprite_sorter() sorts parent sprites
  → ViewportDrawParentSprites() → EmitGpuSpriteCommand() per parent

EmitGpuSpriteCommand (viewport.cpp:1629)
  → Atlas lookup: _sprite_atlas->Get(sprite_id, ZoomLevel::Min)
  → Lazy upload if missing: GetRawSprite() → triggers decode → atlas upload
  → Coordinate transform: screen_x = vp->left + UnScaleByZoom(virt_x - vp->virtual_left, zoom) + UnScaleByZoom(entry.x_offs, zoom)
  → Emit GpuSpriteInstance to command buffer (52 bytes: screen_pos, size, UV, z_depth, mode, tint)

GPU Render (gpu_renderer.cpp)
  → Single render pass with depth buffer (Depth24Plus, compare=Less, clear=1.0)
  → Per-atlas-page: bind page textures → upload instances → draw instanced quads
  → Fragment shader: alpha test (discard if <0.01), remap, transparency, tint
```

## What We Fixed

### Fix 1: Single-Pass GPU Viewport Rendering (commit `a68ed238ea`)

**Problem:** The dirty-block system (64x8 pixel blocks) called `ViewportDoDraw()` ~4 times per frame (coalesced from ~4000 blocks). Sprites at block boundaries were emitted multiple times with different z_depth values, causing non-deterministic depth-buffer ordering.

**Evidence:** Diagnostic showed `viewport_draws=4, emit_total=2440, emit_unique=2180, duplicates=260`

**Fix:** Restructured `Paint()` into two phases:
1. Phase 1: Iterate all viewport windows, call `ViewportDoDraw()` once per viewport with full virtual bounds → single sort, single emission
2. Phase 2: Set `_gpu_suppress_sprite_emit = true` during `DrawDirtyBlocks()` → CPU UI rendering only

**Result:** Duplicates dropped to 0-17. Eliminated the random texture fragment chaos from the original screenshot. BUT exposed underlying issue: missing ground tiles.

**Files:** `src/video/wgpu_v.cpp`, `src/viewport.cpp`, `src/gpu/sprite_command.h`, `src/gpu/sprite_command.cpp`

### Fix 2: Clear Sprite Cache on First GPU Frame (commit `736a06fd66`)

**Problem:** Sprites decoded before the GPU atlas was initialized (during GRF loading, font init) remained in the sprite cache without atlas entries. The lazy upload in `EmitGpuSpriteCommand` called `GetRawSprite()` which returned the cached version without re-triggering `ReadSprite()` (where atlas upload happens).

**Evidence:** Frame 0 diagnostic showed 167/1175 tile sprites with `valid=false` (no atlas entry). By frame 1, all were valid (cache pressure had evicted and re-decoded them).

**Fix:** Call `GfxClearSpriteCache()` on the first `Paint()` call, forcing all sprites to be re-decoded with atlas upload on next access.

**Result:** Zero `reload failed` logs. Atlas uploads happen correctly. BUT visual corruption persists — **this fix may not have been tested with a fresh build** (user's screenshot showed old commit hash in title bar).

**Files:** `src/video/wgpu_v.cpp`

## What Remains Broken

The corruption pattern after Fix 1 (and presumably Fix 2):
- **Ground tile sprites** are invisible (dark navy clear color shows through)
- **Parent sprites** (buildings, vehicles, bridges) render correctly
- The missing tiles create a "floating buildings on dark background" effect
- The issue is spatially biased — bottom portion of viewport tends to be more correct

## Verified NOT the Cause

| Component | Status | How Verified |
|-----------|--------|-------------|
| Atlas key construction | Correct | `MakeKey(id, ZoomLevel::Min)` — consistent upload/lookup |
| WGSL shader + vertex layout | Correct | 52-byte struct, 7 attributes, offsets match exactly |
| Atlas bind groups | Correct | Per-page, cached, correct texture binding |
| Depth buffer config | Correct | Less compare, write enabled, clear to 1.0 |
| Sampler | Correct | Nearest filtering, clamp to edge |
| Instance buffer | Correct | Per-page upload, power-of-2 growth |
| Duplicate emission | Fixed | Single-pass viewport rendering |
| Sprite cache pre-atlas | Fixed | GfxClearSpriteCache on first frame |

## CONFIRMED Root Cause: Atlas Pixel Alpha = 0

**Tested by replacing `discard` with magenta in the fragment shader.** Result: massive magenta areas exactly where dark gaps were. This proves sprites ARE emitted with correct positions and UVs, but the atlas RGBA texture data has `alpha=0` for pixels that should be opaque.

### The bug is in the RGBA conversion during atlas upload (`spritecache.cpp:539-575`):

```cpp
const SpriteLoader::Sprite &src = sprite.Root();  // sprite[ZoomLevel::Min]
// ...
for (uint32_t i = 0; i < pixel_count; i++) {
    const SpriteLoader::CommonPixel &px = src.data[i];
    // ... RGB conversion ...
    rgba[i * 4 + 3] = px.a;    // <-- THIS IS 0 FOR MANY OPAQUE PIXELS
    m_data[i] = px.m;
}
```

`px.a` (the alpha channel of `sprite[ZoomLevel::Min]`) is 0 for pixels that should be opaque.

### Most likely sub-causes (investigate in this order):

**1. `ResizeSpriteIn()` doesn't preserve alpha during upscale to ZoomLevel::Min**
Most base graphics are provided at ZoomLevel::Normal. `ResizeSprites()` calls `ResizeSpriteIn()` to upscale to ZoomLevel::Min. If the upscaling copies RGB but doesn't copy alpha, the Min-level data would have `a=0` everywhere.

Where to look: `src/spritecache.cpp` — the `ResizeSpriteIn()` function. Check if it copies `CommonPixel.a` from source pixels.

**2. Sprite loader doesn't set alpha for 8bpp sprites at certain zoom levels**
For 8bpp (palette-only) sprites, the GRF loader might not set `px.a = 255` for opaque pixels at all zoom levels. The alpha might only be set for the natively-provided zoom level.

Where to look: `src/spriteloader/grf.cpp` — check how `CommonPixel.a` is initialized for each zoom level.

**3. `PadSprites()` pads with zero-alpha pixels that overlap the visible sprite area**
PadSprites adjusts sprite dimensions for zoom-level alignment. If the padding is done by allocating a new buffer zeroed to 0 and copying pixel data with wrong offsets, the visible area could be overwritten with zeros.

Where to look: `src/spritecache.cpp` — `PadSprites()` function, specifically buffer allocation and copy logic.

### Quickest fix to try

In the atlas upload code (`spritecache.cpp:539-575`), temporarily force alpha=255 for opaque pixels and check if rendering becomes correct:

```cpp
// After the pixel conversion loop, force alpha for pixels with non-zero palette index:
for (uint32_t i = 0; i < pixel_count; i++) {
    if (m_data[i] != 0 && rgba[i * 4 + 3] == 0) {
        rgba[i * 4 + 3] = 255;  // Force opaque for palette-indexed pixels
    }
}
```

If this fixes rendering, the root cause is confirmed as alpha not being set for opaque pixels at ZoomLevel::Min. Then trace backwards to find WHERE alpha should have been set.

### Other hypotheses (ruled out)

| Hypothesis | Status | Evidence |
|-----------|--------|----------|
| H1: Wrong alpha in atlas | **CONFIRMED** | Magenta shader test |
| H2: Wrong coordinates | Ruled out | Magenta shows sprites at correct positions |
| H3: Depth buffer conflict | Ruled out | Magenta shows tiles ARE rendered, just transparent |
| H4: Tile collection differs | Ruled out | Frame 1 diagnostic: 552/552 tiles valid |
| H5: UV precision | Ruled out | Magenta covers entire sprite areas, not edges |

## Key Files

| File | Key Functions | Purpose |
|------|--------------|---------|
| `src/video/wgpu_v.cpp` | `Paint()`, `RenderFrame()` | Frame loop, two-phase rendering |
| `src/viewport.cpp` | `EmitGpuSpriteCommand()`, `ViewportDrawTileSprites()`, `ViewportDrawParentSprites()`, `ViewportDoDraw()` | GPU sprite emission |
| `src/gpu/sprite_atlas.cpp` | `Upload()`, `Get()`, `FindSpace()` | Atlas packing and lookup |
| `src/gpu/gpu_renderer.cpp` | `SubmitSprites()`, `EnsureAtlasBindGroups()` | GPU render pass |
| `src/gpu/sprite_command.h/.cpp` | `SpriteCommandBuffer`, `GpuSpriteInstance` | Command buffer + instance struct |
| `src/spritecache.cpp:539-575` | Atlas upload hook in `ReadSprite()` | RGBA conversion + upload |
| `src/spriteloader/spriteloader.hpp` | `Root()`, `SpriteCollection` | Zoom-level sprite data |

## Quick Debug Commands

```bash
# Build and run with GPU debug logging
cd build && make -j$(sysctl -n hw.ncpu)
./openttd -v wgpu -d driver=1,sprite=1

# Check for atlas issues
grep "atlas.*failed\|mismatch\|reload failed" log.txt

# Key sprite IDs to watch
# Sprite 2771 — known metadata mismatch (116x36 vs 116x48)
# Sprite 3996, 4505, 4524, 4525, 3990 — common ground tiles
```

## Commit History on Branch

```
736a06fd66 Fix: clear sprite cache on first GPU frame to force atlas population
e760f5fe4e Cleanup: remove GPU diagnostic instrumentation
a68ed238ea Fix: single-pass GPU viewport rendering eliminates dirty-block duplicate emission
f1d8f7847f Codechange: add GPU sprite emission diagnostics
59d58ec2d9 Fix: zoom-scale SubSprite clip bounds + re-enable CPU sort
```

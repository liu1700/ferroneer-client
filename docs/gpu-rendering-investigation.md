# GPU Rendering Corruption — Investigation Summary

**Branch:** `phase3/gpu-visual-fixes`
**Date:** 2026-03-29
**Status:** Partially fixed, core visual corruption remains

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

## Hypotheses for Remaining Bug

### H1: Atlas texture data has wrong alpha for ground tiles
Ground tile sprites might be uploaded with alpha=0 in their RGBA data, causing the fragment shader to `discard` them. This could happen if:
- The palette-to-RGB conversion produces wrong alpha for 8bpp sprites
- The upscaled ZoomLevel::Min data has alpha artifacts from `ResizeSpriteIn()`

**How to test:** Dump a few tile sprite atlas entries' pixel data, or add a debug mode that forces alpha=1.0 in the fragment shader.

### H2: Coordinate transform places tiles off-screen
The `UnScaleByZoom(entry.x_offs/y_offs, zoom)` transform might produce wrong screen positions for tile sprites specifically. Ground tiles use large negative offsets (e.g., -124, -64 at ZoomLevel::Min).

**How to test:** Log screen_x/screen_y for tile sprites and verify they're within viewport bounds.

### H3: Z-depth conflict between tile sprites and invisible parent children
Some parent sprites use `SPR_EMPTY_BOUNDING_BOX` (invisible parent) with child sprites. These children get z_depth in the parent range (0.001-0.499), which wins the depth test over tiles (0.5-0.999). If children have partially transparent pixels (alpha > 0.01 but visually invisible), they'd block tiles.

**How to test:** Temporarily disable parent sprite rendering and see if tiles appear.

### H4: ViewportAddLandscape coverage differs in single-pass mode
The single-pass `ViewportDoDraw()` with full viewport bounds might set up `_vd.dpi` differently than the dirty-block path, causing `ViewportAddLandscape()` to miss some tiles. The title screen has 5 viewport windows; some might overlap and interfere.

**How to test:** Compare tile collection counts between Phase 1 (single-pass) and the original dirty-block path.

### H5: Sprite atlas UV precision or texture upload alignment
The `wgpuQueueWriteTexture` row alignment (256 bytes) might cause pixel data to be offset within the atlas page, making UVs point to slightly wrong regions. For small sprites this could mean sampling transparent padding.

**How to test:** Add 1-pixel border validation around atlas entries, or dump atlas pages as PNG for visual inspection.

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

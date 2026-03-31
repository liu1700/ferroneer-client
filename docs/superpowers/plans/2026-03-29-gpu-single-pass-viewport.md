# GPU Single-Pass Viewport Rendering — Fix Dirty-Block Duplicate Emission

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix GPU rendering corruption caused by the same sprite being emitted thousands of times per frame with inconsistent z_depth values.

**Architecture:** The dirty-block system (64×8 pixel blocks, ~4000 per frame) calls `ViewportDoDraw()` once per block, each time independently collecting, sorting, and emitting sprites to the GPU command buffer. A sprite spanning multiple blocks gets emitted N times with different z_depth values. The GPU depth buffer non-deterministically picks winners, producing wrong overlap ordering. The fix: emit viewport sprites to the GPU command buffer exactly once per frame per viewport, with a single global sort, then suppress GPU emission during the subsequent per-block `DrawDirtyBlocks()` pass.

**Tech Stack:** C++20, wgpu-native, OpenTTD viewport/blitter architecture

---

## Background: Why This Happens

| Component | CPU path (works) | GPU path (broken) |
|-----------|-------------------|-------------------|
| Dirty blocks | 64×8 px → ~4050 blocks/frame | Same |
| ViewportDoDraw per block | Collects sprites in block region, sorts, blitter clips to block bounds, last-draw-wins | Collects sprites in block region, sorts, emits to GPU with `z_depth = base - range*(idx/count)`, NO clip |
| Sprite overlap | Painter's algorithm (overdraw is fine) | Depth test — smallest z_depth wins |
| Multi-block sprites | Each block draws only its portion — consistent | Same sprite emitted N times with DIFFERENT z_depth (idx and count vary per block) |

### Key numbers

For a 1920×1080 screen at Normal zoom:
- Dirty blocks: `ceil(1920/64) × ceil(1080/8)` = **30 × 135 = 4050** blocks
- A 32×16 ground tile spans `ceil(16/8)` = **2 blocks** vertically
- A 64×48 building spans `ceil(48/8)` = **6 blocks** vertically, possibly 2 horizontally = **~12 blocks**
- The command buffer has **4-12× more instances** than actual unique sprites
- Each instance has a different z_depth → depth-buffer chaos

## Key Files

| File | Role | Changes |
|------|------|---------|
| `src/video/wgpu_v.cpp` | `Paint()` entry point | Add single-pass GPU viewport draw before `DrawDirtyBlocks()` |
| `src/viewport.cpp` | `ViewportDoDraw()`, `ViewportDrawTileSprites()`, `ViewportDrawParentSprites()`, `EmitGpuSpriteCommand()` | Add emission suppression flag + diagnostic counters |
| `src/gpu/sprite_command.h` | `SpriteCommandBuffer`, `_gpu_command_buffer` global | Add `_gpu_suppress_sprite_emit` extern declaration |
| `src/gpu/sprite_command.cpp` | Command buffer implementation | Define `_gpu_suppress_sprite_emit` |
| `src/viewport_func.h` | `ViewportDoDraw()` declaration | Already declared — no change |

---

## Task 1: Diagnostic Instrumentation — Verify Duplicate Emissions

**Goal:** Before implementing any fix, prove the hypothesis by measuring duplicate sprite emission and z_depth variance. This is Phase 1 (Root Cause Investigation) of the debugging process.

**Files:**
- Modify: `src/viewport.cpp` (EmitGpuSpriteCommand, ~line 1629)
- Modify: `src/video/wgpu_v.cpp` (Paint, line 712)

### Diagnostics to add

We'll add per-frame counters and periodic (every 120 frames / ~2 seconds) log output showing:
- **Total GPU sprite emissions** per frame
- **Unique (sprite_id, virt_x, virt_y) tuples** per frame
- **Duplicate count** = total - unique
- **ViewportDoDraw call count** per frame

- [ ] **Step 1: Add frame counters in viewport.cpp**

At file scope near the top of the `#ifdef WITH_WGPU` section around line 1623, add:

```cpp
#ifdef WITH_WGPU
/* --- GPU diagnostic counters (temporary) --- */
static uint32_t _gpu_diag_emit_total = 0;
static uint32_t _gpu_diag_emit_unique = 0;
static uint32_t _gpu_diag_viewport_draws = 0;
static std::unordered_set<uint64_t> _gpu_diag_seen;

void GpuDiagResetFrame()
{
	_gpu_diag_emit_total = 0;
	_gpu_diag_emit_unique = 0;
	_gpu_diag_viewport_draws = 0;
	_gpu_diag_seen.clear();
}

void GpuDiagLogFrame(uint32_t frame_num)
{
	if (frame_num % 120 != 0) return;
	Debug(driver, 0,
		"[gpu][diag] frame={} viewport_draws={} emit_total={} emit_unique={} duplicates={}",
		frame_num, _gpu_diag_viewport_draws, _gpu_diag_emit_total, _gpu_diag_emit_unique,
		_gpu_diag_emit_total - _gpu_diag_emit_unique);
}
```

- [ ] **Step 2: Instrument EmitGpuSpriteCommand**

Inside `EmitGpuSpriteCommand()` (viewport.cpp ~line 1636), right after `SpriteID sprite_id = image & SPRITE_MASK;`, add:

```cpp
	_gpu_diag_emit_total++;
	uint64_t diag_key = (static_cast<uint64_t>(sprite_id) << 40)
	                   | (static_cast<uint64_t>(static_cast<uint32_t>(virt_x) & 0xFFFFF) << 20)
	                   | static_cast<uint64_t>(static_cast<uint32_t>(virt_y) & 0xFFFFF);
	if (_gpu_diag_seen.insert(diag_key).second) {
		_gpu_diag_emit_unique++;
	}
```

- [ ] **Step 3: Instrument ViewportDoDraw**

Inside `ViewportDoDraw()` (viewport.cpp ~line 2016), right after the opening brace, add:

```cpp
#ifdef WITH_WGPU
	if (_gpu_command_buffer != nullptr) _gpu_diag_viewport_draws++;
#endif
```

- [ ] **Step 4: Add forward declarations to viewport_func.h or use a local extern**

In `wgpu_v.cpp`, before `Paint()`, add:

```cpp
#ifdef WITH_WGPU
extern void GpuDiagResetFrame();
extern void GpuDiagLogFrame(uint32_t frame_num);
#endif
```

- [ ] **Step 5: Call diagnostics from Paint()**

In `VideoDriver_Wgpu::Paint()` (wgpu_v.cpp ~line 712), add counter management:

```cpp
void VideoDriver_Wgpu::Paint()
{
	PerformanceMeasurer framerate(PFE_VIDEO);

#ifdef WITH_WGPU
	static uint32_t gpu_frame_counter = 0;
	GpuDiagResetFrame();
#endif

	this->command_buffer.Reset();
	std::fill(this->video_buffer.begin(), this->video_buffer.end(), 0);
	MarkWholeScreenDirty();
	DrawDirtyBlocks();

#ifdef WITH_WGPU
	GpuDiagLogFrame(gpu_frame_counter++);
#endif

	this->RenderFrame();
}
```

- [ ] **Step 6: Build and run**

```bash
cd /Users/yuchenliu/Documents/ferroneer/ferroneer-client/build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo && make -j$(sysctl -n hw.ncpu)
./openttd -d driver=1
```

- [ ] **Step 7: Analyze log output**

Expected output (confirming hypothesis):
```
[gpu][diag] frame=120 viewport_draws=4050 emit_total=85000 emit_unique=12000 duplicates=73000
```

**Decision gate:**
- If `duplicates > 0` AND `viewport_draws >> 1`: **Hypothesis CONFIRMED** → proceed to Task 2
- If `duplicates == 0`: Hypothesis wrong → the issue is elsewhere, investigate atlas metadata mismatch path instead
- If `viewport_draws == 1`: Dirty blocks already coalesced → issue is within single-pass sorting, investigate z_depth formula

- [ ] **Step 8: Commit diagnostics**

```bash
git add src/viewport.cpp src/video/wgpu_v.cpp
git commit -m "Codechange: add GPU sprite emission diagnostics for dirty-block investigation"
```

---

## Task 2: Implement Single-Pass GPU Viewport Rendering

**Goal:** Emit GPU sprites once per viewport per frame, using a single global sort, then suppress GPU emission during `DrawDirtyBlocks()`.

**Files:**
- Modify: `src/gpu/sprite_command.h` (add extern flag)
- Modify: `src/gpu/sprite_command.cpp` (define flag)
- Modify: `src/video/wgpu_v.cpp` (restructure Paint())
- Modify: `src/viewport.cpp` (check suppress flag in EmitGpuSpriteCommand)

### Design

The `Paint()` function becomes two-phase:

```
Phase 1 — GPU sprites:  For each viewport, call ViewportDoDraw() once with
           FULL viewport virtual bounds.  This collects ALL sprites in the
           viewport, sorts them globally, and emits once to the command buffer.

Phase 2 — CPU UI:       Set _gpu_suppress_sprite_emit = true, then run
           MarkWholeScreenDirty() + DrawDirtyBlocks() as before.  Viewport
           drawing still runs (for signs, text, overlays) but GPU emission
           in EmitGpuSpriteCommand() early-returns.  UI windows draw
           normally to the CPU video buffer.

Phase 3 — Render:       SubmitSprites + CompositeUI + Present, then reset.
```

Why this works:
- Phase 1 produces ONE emission per sprite with correct z_depth from the global sort
- Phase 2 renders signs/text/overlays to the CPU buffer (these are drawn outside `ViewportDrawTileSprites`/`ViewportDrawParentSprites`, so the suppress flag doesn't affect them)
- Phase 2's `ViewportDrawTileSprites`/`ViewportDrawParentSprites` check `_gpu_command_buffer != nullptr` → take GPU branch → check suppress flag → skip emission → `return` (skip CPU draw too, which is correct — GPU handles world sprites)
- The video buffer has: transparent pixels where the viewport is, opaque pixels where UI is — same as current behavior

- [ ] **Step 1: Add the suppress flag in sprite_command.h**

In `src/gpu/sprite_command.h`, after the `extern SpriteCommandBuffer *_gpu_command_buffer;` line (~line 87), add:

```cpp
/** When true, EmitGpuSpriteCommand skips emission (used during DrawDirtyBlocks phase). */
extern bool _gpu_suppress_sprite_emit;
```

- [ ] **Step 2: Define the flag in sprite_command.cpp**

In `src/gpu/sprite_command.cpp`, near the existing global definitions, add:

```cpp
bool _gpu_suppress_sprite_emit = false;
```

- [ ] **Step 3: Check the flag in EmitGpuSpriteCommand**

In `src/viewport.cpp`, at the top of `EmitGpuSpriteCommand()` (~line 1634), right after the existing null checks, add:

```cpp
static void EmitGpuSpriteCommand(SpriteID image, PaletteID pal,
	int virt_x, int virt_y, float z_depth,
	const DrawPixelInfo *dpi, const Viewport *vp,
	const SubSprite *sub)
{
	if (_gpu_command_buffer == nullptr || _sprite_atlas == nullptr) return;
	if (_gpu_suppress_sprite_emit) return;
```

- [ ] **Step 4: Restructure Paint() for two-phase rendering**

In `src/video/wgpu_v.cpp`, add the viewport include near the other includes (~line 22):

```cpp
#include "../viewport_func.h"
#include "../window_gui.h"
```

Then replace the existing `Paint()` method body:

```cpp
void VideoDriver_Wgpu::Paint()
{
	PerformanceMeasurer framerate(PFE_VIDEO);

	this->command_buffer.Reset();
	std::fill(this->video_buffer.begin(), this->video_buffer.end(), 0);

#ifdef WITH_WGPU
	/* Phase 1 — GPU sprites: draw each viewport in a single pass so every
	 * sprite is collected once, sorted once, and emitted once with a
	 * globally consistent z_depth.  This eliminates the per-dirty-block
	 * duplicate emissions (64×8 blocks → ~4000 passes otherwise). */
	if (_gpu_command_buffer != nullptr) {
		for (const Window *w : Window::Iterate()) {
			if (w->viewport == nullptr) continue;
			const Viewport &vp = *w->viewport;
			ViewportDoDraw(vp,
				vp.virtual_left,
				vp.virtual_top,
				vp.virtual_left + vp.virtual_width,
				vp.virtual_top + vp.virtual_height);
		}
	}

	/* Phase 2 — CPU UI: suppress GPU sprite emission so DrawDirtyBlocks
	 * only produces CPU-rendered content (signs, text, overlays, UI). */
	_gpu_suppress_sprite_emit = true;
#endif

	MarkWholeScreenDirty();
	DrawDirtyBlocks();

#ifdef WITH_WGPU
	_gpu_suppress_sprite_emit = false;

	/* Keep diagnostics from Task 1 for verification. */
	static uint32_t gpu_frame_counter = 0;
	GpuDiagResetFrame();  /* Reset will show 0 for this frame — that's OK, remove later. */
	GpuDiagLogFrame(gpu_frame_counter++);
#endif

	this->RenderFrame();
}
```

- [ ] **Step 5: Build**

```bash
cd /Users/yuchenliu/Documents/ferroneer/ferroneer-client/build
make -j$(sysctl -n hw.ncpu)
```

Expected: compiles cleanly. If `window_gui.h` include causes issues (it's a large header), use `window_func.h` + a forward declaration of `Window` instead. The `Window::Iterate()` API needs `window_gui.h`.

- [ ] **Step 6: Run and visually verify**

```bash
./openttd -d driver=1
```

**What to check:**
1. Main menu renders correctly — no black blocks, no random texture fragments
2. Start a new game, zoom in/out with scroll wheel — scene stays visible, no disappearance
3. At maximum zoom out — no large-scale corruption or atlas artifacts
4. Scroll around the map — no state pollution or persistent artifacts
5. Check that UI elements (menus, toolbars, signs, town names) still render correctly on the CPU layer

- [ ] **Step 7: Commit**

```bash
git add src/gpu/sprite_command.h src/gpu/sprite_command.cpp src/viewport.cpp src/video/wgpu_v.cpp
git commit -m "Fix: single-pass GPU viewport rendering eliminates dirty-block duplicate emission

The 64x8 dirty-block system called ViewportDoDraw ~4000 times per frame,
each time independently collecting, sorting, and emitting sprites to the
GPU command buffer. Sprites spanning multiple blocks were emitted N times
with different z_depth values, causing non-deterministic depth-buffer
outcomes (wrong overlap ordering, black holes, texture fragments).

Now Paint() draws each viewport once with full bounds (single global sort,
one emission per sprite), then suppresses GPU emission during
DrawDirtyBlocks (which still runs for CPU UI content)."
```

---

## Task 3: Remove Diagnostics + Final Verification

**Goal:** Clean up the temporary diagnostic code from Task 1, verify the fix is solid.

**Files:**
- Modify: `src/viewport.cpp` (remove diagnostic counters)
- Modify: `src/video/wgpu_v.cpp` (remove diagnostic calls)

- [ ] **Step 1: Remove diagnostic code from viewport.cpp**

Remove the entire diagnostic block added in Task 1 Step 1:
- The `_gpu_diag_*` variables
- `GpuDiagResetFrame()` function
- `GpuDiagLogFrame()` function
- The `#include <unordered_set>` if it was added only for diagnostics

Remove the diagnostic tracking in `EmitGpuSpriteCommand()` added in Task 1 Step 2:
- The `_gpu_diag_emit_total++` line
- The `diag_key` / `_gpu_diag_seen.insert` block

Remove the counter increment in `ViewportDoDraw()` added in Task 1 Step 3.

- [ ] **Step 2: Remove diagnostic code from wgpu_v.cpp**

Remove from `Paint()`:
- The `extern void GpuDiagResetFrame()` and `GpuDiagLogFrame()` declarations
- The `static uint32_t gpu_frame_counter` variable
- The `GpuDiagResetFrame()` and `GpuDiagLogFrame()` calls

- [ ] **Step 3: Build and run final verification**

```bash
cd /Users/yuchenliu/Documents/ferroneer/ferroneer-client/build
make -j$(sysctl -n hw.ncpu)
./openttd
```

**Full test checklist:**
1. Main menu: background world renders correctly, no corruption
2. New game at default zoom: terrain, buildings, vehicles visible and correctly layered
3. Zoom IN (scroll up): scene stays visible, no disappearance or flash
4. Zoom OUT to maximum: no atlas-like corruption, no grey/red/random patches
5. Zoom out then zoom back in: no persistent artifacts (state pollution gone)
6. Scroll map while zoomed out: no flickering or delayed rendering
7. Open/close windows (build menu, vehicle list): UI overlays correctly on world view
8. Town names and signs: visible and correctly positioned at all zoom levels
9. Company-colored vehicles: remap colors appear correct (not white/black)
10. Building placement preview: green/red tint visible when placing structures

- [ ] **Step 4: Commit final cleanup**

```bash
git add src/viewport.cpp src/video/wgpu_v.cpp
git commit -m "Cleanup: remove GPU diagnostic instrumentation after single-pass fix verified"
```

---

## Task 4 (If Needed): Address Atlas Metadata Mismatch

**Only if visual issues persist after Task 2/3.** The metadata mismatch log (`[gpu][atlas] reusing existing entry... despite metadata mismatch`) indicates a secondary issue: the same sprite is decoded with different dimensions at different times.

**Root cause (secondary):** When the CPU sprite cache evicts and re-decodes a sprite, `ResizeSprites()` + `PadSprites()` may produce slightly different root dimensions due to rounding during upscale/downscale cycles (especially when `_settings_client.gui.sprite_zoom_min` is active). The atlas returns the first entry (correct behavior), but the log signals an inconsistency.

**Files:**
- Modify: `src/spritecache.cpp` (~line 539-575, the `#ifdef WITH_WGPU` atlas upload block)

- [ ] **Step 1: Add evict-before-reupload logic**

If a sprite is re-decoded and the atlas already has an entry with different metadata, evict the old entry and re-upload with the new data:

In `src/spritecache.cpp`, replace the upload block (~line 539-575):

```cpp
#ifdef WITH_WGPU
	if (_sprite_atlas != nullptr && sprite_type == SpriteType::Normal) {
		const SpriteLoader::Sprite &src = sprite.Root();
		if (src.width != 0 && src.height != 0 && src.data != nullptr) {
			/* Check if existing atlas entry has mismatched dimensions.
			 * This can happen when sprite cache evicts and re-decodes
			 * a sprite with slightly different PadSprites() results. */
			AtlasEntry existing = _sprite_atlas->Get(id, ZoomLevel::Min);
			if (existing.valid &&
					(existing.width != src.width || existing.height != src.height ||
					 existing.x_offs != src.x_offs || existing.y_offs != src.y_offs)) {
				_sprite_atlas->Evict(id);
			}

			uint32_t pixel_count = static_cast<uint32_t>(src.width) * src.height;
			std::vector<uint8_t> rgba(pixel_count * 4);
			std::vector<uint8_t> m_data(pixel_count);
			bool has_rgb = src.colours.Test(SpriteComponent::RGB);

			for (uint32_t i = 0; i < pixel_count; i++) {
				const SpriteLoader::CommonPixel &px = src.data[i];
				if (has_rgb) {
					rgba[i * 4 + 0] = px.r;
					rgba[i * 4 + 1] = px.g;
					rgba[i * 4 + 2] = px.b;
				} else {
					Colour c = _cur_palette.palette[px.m];
					rgba[i * 4 + 0] = c.r;
					rgba[i * 4 + 1] = c.g;
					rgba[i * 4 + 2] = c.b;
				}
				rgba[i * 4 + 3] = px.a;
				m_data[i] = px.m;
			}

			AtlasEntry entry = _sprite_atlas->Upload(id, rgba.data(), m_data.data(),
				src.width, src.height, src.x_offs, src.y_offs, ZoomLevel::Min);
			if (!entry.valid) {
				Debug(sprite, 0, "atlas: failed to upload sprite {}", id);
			}
		}
	}
#endif
```

Note: `Evict()` removes the lookup entry but does NOT reclaim GPU texture memory (shelf packing is append-only). The new upload allocates a fresh slot. This is acceptable for the small number of re-decoded sprites. If atlas memory exhaustion becomes a concern, a full atlas rebuild would be needed.

- [ ] **Step 2: Build and verify**

```bash
cd /Users/yuchenliu/Documents/ferroneer/ferroneer-client/build
make -j$(sysctl -n hw.ncpu)
./openttd -d driver=1
```

Verify: no more `[gpu][atlas] reusing existing entry... despite metadata mismatch` log messages.

- [ ] **Step 3: Commit**

```bash
git add src/spritecache.cpp
git commit -m "Fix: evict stale atlas entries on sprite re-decode metadata mismatch"
```

---

## Summary

| Task | Purpose | Risk | Time |
|------|---------|------|------|
| 1 | Diagnostic proof | None (logging only) | 5 min |
| 2 | Core fix: single-pass emission | Medium (changes Paint flow) | 15 min |
| 3 | Cleanup diagnostics | None | 5 min |
| 4 | Secondary: atlas mismatch | Low (only if issues persist) | 10 min |

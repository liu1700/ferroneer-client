# Road Stop Preview Validation + Direction Arrow — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show green/red tint based on buildability and arrow sprites for entrance direction when placing truck/bus stations.

**Architecture:** Expand `RoadStopPreviewInfo` to carry dry-run parameters, execute `CMD_BUILD_ROAD_STOP` dry-run in `DrawTileSelection`, and draw `SPR_ARROW_*` sprites based on orientation. All changes in two files: `road_gui.cpp` (data) and `viewport.cpp` (rendering).

**Tech Stack:** C++20, OpenTTD command system, existing GPU tint rendering

---

## File Structure

| File | Change | Responsibility |
|------|--------|----------------|
| `src/road_gui.cpp` | Modify | Expand `RoadStopPreviewInfo` struct + `GetRoadStopPlacementPreview()` to include dry-run params |
| `src/road_gui.h` | Modify | Update `RoadStopPreviewInfo` struct declaration |
| `src/viewport.cpp` | Modify | Dry-run validation in `DrawTileSelection`, arrow sprite drawing |

---

## Task 1: Expand RoadStopPreviewInfo with Dry-Run Parameters

**Goal:** The current `RoadStopPreviewInfo` only has `active`, `is_bus`, `orientation`. The dry-run needs: road type, spec class, spec index, and the raw DiagDirection (not the gfx uint8_t). Add these fields.

**Files:**
- Modify: `src/road_gui.h` (struct definition)
- Modify: `src/road_gui.cpp:84-110` (GetRoadStopPlacementPreview)

- [ ] **Step 1: Update RoadStopPreviewInfo struct in road_gui.h**

Find the struct definition and add the new fields:

```cpp
struct RoadStopPreviewInfo {
	bool active;             ///< Whether a road stop picker is currently open.
	bool is_bus;             ///< True for bus stop, false for truck stop.
	uint8_t orientation;     ///< Station gfx index (0-3 bay, 4-5 drive-through).
	DiagDirection ddir;      ///< Entrance direction (adjusted for bay vs drive-through).
	bool is_drive_through;   ///< True if drive-through orientation selected.
	RoadType road_type;      ///< Current road type being built.
	RoadStopClassID spec_class; ///< Station spec class (default or NewGRF).
	uint16_t spec_index;     ///< Station spec index within class.
};
```

You'll need to add includes at the top of `road_gui.h` if not already present:
```cpp
#include "road_type.h"
#include "newgrf_roadstop.h"
```

Check what's already included before adding — avoid duplicates.

- [ ] **Step 2: Populate new fields in GetRoadStopPlacementPreview**

In `src/road_gui.cpp`, function `GetRoadStopPlacementPreview()` (~line 84-110). After setting `info.orientation`, add:

```cpp
	info.active = true;
	info.is_bus = is_bus;
	info.orientation = static_cast<uint8_t>(_roadstop_gui.orientation);

	/* Parameters needed for CMD_BUILD_ROAD_STOP dry-run validation. */
	DiagDirection ddir = _roadstop_gui.orientation;
	info.is_drive_through = (ddir >= DIAGDIR_END);
	info.ddir = info.is_drive_through ? static_cast<DiagDirection>(ddir - DIAGDIR_END) : ddir;
	info.road_type = _cur_roadtype;
	info.spec_class = _roadstop_gui.sel_class;
	info.spec_index = _roadstop_gui.sel_type;
```

Note: `_cur_roadtype` is a global defined in `src/road_cmd.h` or `src/road_internal.h`. Check if it's accessible from `road_gui.cpp` — it should be since `PlaceRoadStop()` in the same file already uses `rt` parameter. If `_cur_roadtype` isn't directly accessible, look at how `BuildRoadStopWindow::OnPlaceObject` passes the road type (line ~312) and replicate that.

- [ ] **Step 3: Build and verify compilation**

```bash
cd /Users/yuchenliu/Documents/ferroneer/ferroneer-client/build
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -5
```

- [ ] **Step 4: Commit**

```bash
git add src/road_gui.h src/road_gui.cpp
git commit -m "Codechange: expand RoadStopPreviewInfo with dry-run parameters"
```

---

## Task 2: Dry-Run Validation (Green/Red Tint)

**Goal:** Execute `CMD_BUILD_ROAD_STOP` dry-run on the cursor tile during hover. Green tint = buildable, red = not buildable.

**Files:**
- Modify: `src/viewport.cpp:1150-1175` (DrawTileSelection preview block)

- [ ] **Step 1: Add required includes to viewport.cpp**

Near the top of viewport.cpp, add these includes if not already present:

```cpp
#include "command_func.h"
#include "station_cmd.h"
#include "road_map.h"
```

These are needed for `Command<Commands::BuildRoadStop>::Do()` and `RoadStopType`.

- [ ] **Step 2: Replace the preview_pal logic in DrawTileSelection**

In `src/viewport.cpp`, inside the `DrawTileSelection` function, find the preview block (~line 1150-1175). The current code sets `preview_pal` based on `_thd.make_square_red`. Replace the entire preview block contents (inside the `if (_thd.drawstyle & HT_RECT)` block, before the selection border drawing):

```cpp
			{
				RoadStopPreviewInfo preview = GetRoadStopPlacementPreview();
				if (preview.active) {
					/* Dry-run the build command to check if this tile is buildable. */
					RoadStopType stop_type = preview.is_bus ? RoadStopType::Bus : RoadStopType::Truck;
					CommandCost cost = Command<Commands::BuildRoadStop>::Do(
						DoCommandFlag::Auto,
						ti->tile,
						1, 1,  /* width, length — single tile */
						stop_type,
						preview.is_drive_through,
						preview.ddir,
						preview.road_type,
						preview.spec_class,
						preview.spec_index,
						StationID::Invalid(),
						true   /* adjacent */
					);
					bool can_build = cost.Succeeded();

					StationType st = preview.is_bus ? StationType::Bus : StationType::Truck;
					const DrawTileSprites *t = GetStationTileLayout(st, preview.orientation);

					PaletteID preview_pal = can_build ? PALETTE_TO_STRUCT_BLUE : PALETTE_TO_STRUCT_RED;
					for (const DrawTileSeqStruct &dtss : t->GetSequence()) {
						SpriteID image = dtss.image.sprite;
						if (GB(image, 0, SPRITE_WIDTH) == 0) continue;

						SpriteID timg = image;
						SetBit(timg, PALETTE_MODIFIER_TRANSPARENT);
						AddTileSpriteToDraw(timg, preview_pal,
							ti->x + dtss.origin.x, ti->y + dtss.origin.y, ti->z + dtss.origin.z);
					}
				}
			}
```

Key changes from old code:
- `preview_pal` now depends on `cost.Succeeded()` (dry-run result) instead of `_thd.make_square_red`
- The `DoCommandFlag::Auto` flag runs validation only, no execution
- `adjacent = true` allows joining adjacent stations (most permissive validation)

- [ ] **Step 3: Build**

```bash
cd /Users/yuchenliu/Documents/ferroneer/ferroneer-client/build
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -10
```

If build fails due to missing includes or wrong types, check:
- `Commands::BuildRoadStop` needs `#include "station_cmd.h"`
- `DoCommandFlag::Auto` needs `#include "command_func.h"`
- `RoadStopType` needs `#include "station_type.h"` or `"road_map.h"`
- `CommandCost` needs `#include "command_type.h"`

- [ ] **Step 4: Manual test**

```bash
./openttd -v wgpu
```

1. New game → open road toolbar → truck station
2. Hover over **flat grass** → should see **green** tinted building preview
3. Hover over **water** → should see **red** tinted preview
4. Hover over **existing building** → should see **red** tinted preview
5. Hover over **steep slope** → should see **red** tinted preview

- [ ] **Step 5: Commit**

```bash
git add src/viewport.cpp
git commit -m "Feature: green/red road stop preview based on dry-run build validation"
```

---

## Task 3: Arrow Direction Indicator

**Goal:** Draw arrow sprites on the station tile showing entrance direction. Bay stops get one arrow, drive-through stops get two.

**Files:**
- Modify: `src/viewport.cpp:1150-1175` (DrawTileSelection, same preview block)

- [ ] **Step 1: Add arrow drawing after building preview sprites**

In `src/viewport.cpp`, inside the `DrawTileSelection` preview block, AFTER the building sprite loop (the `for (const DrawTileSeqStruct &dtss : ...)` loop) but still inside the `if (preview.active)` block, add:

```cpp
					/* Draw entrance direction arrow(s) on the station tile.
					 * Uses existing SPR_ARROW_* UI sprites as directional indicators.
					 * Bay stops: 1 arrow (entrance direction).
					 * Drive-through stops: 2 arrows (both directions). */
					static const SpriteID arrow_for_diagdir[] = {
						SPR_ARROW_RIGHT, /* DIAGDIR_NE → upper-right on screen */
						SPR_ARROW_DOWN,  /* DIAGDIR_SE → lower-right on screen */
						SPR_ARROW_LEFT,  /* DIAGDIR_SW → lower-left on screen */
						SPR_ARROW_UP,    /* DIAGDIR_NW → upper-left on screen */
					};

					SpriteID arrow = arrow_for_diagdir[preview.ddir];
					AddTileSpriteToDraw(arrow, preview_pal, ti->x, ti->y, ti->z);

					if (preview.is_drive_through) {
						/* Opposite direction arrow for the other end. */
						DiagDirection opposite = ReverseDiagDir(preview.ddir);
						SpriteID arrow_opp = arrow_for_diagdir[opposite];
						AddTileSpriteToDraw(arrow_opp, preview_pal, ti->x, ti->y, ti->z);
					}
```

Note: `ReverseDiagDir()` is a standard OpenTTD helper in `src/direction_func.h` that returns the opposite DiagDirection (NE↔SW, SE↔NW). It should already be included via the viewport.cpp include chain.

The arrows use the same `preview_pal` (green or red) as the building sprites, so they match the validation color.

- [ ] **Step 2: Build**

```bash
cd /Users/yuchenliu/Documents/ferroneer/ferroneer-client/build
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -5
```

If `ReverseDiagDir` is not found, add `#include "direction_func.h"` to viewport.cpp.

- [ ] **Step 3: Manual test — verify arrow directions**

```bash
./openttd -v wgpu
```

1. New game → road toolbar → truck station
2. Select **bay orientation 0** (top-left in picker grid) → hover over tile → one arrow pointing right (NE entrance)
3. Press **R to rotate** → arrow direction changes with each rotation
4. Select **drive-through orientation** (bottom row in picker grid) → hover → two arrows pointing opposite directions
5. Verify arrows are green on buildable tiles, red on unbuildable tiles
6. If arrow direction doesn't match the visual entrance of the station building, adjust the `arrow_for_diagdir` mapping

**If the mapping is wrong:** The isometric projection makes direction ambiguous. Try swapping pairs:
- If NE should be UP not RIGHT: swap entries 0 and 3
- If SE should be RIGHT not DOWN: swap entries 1 and 0
- Verify by building an actual station and watching which way the truck enters

- [ ] **Step 4: Commit**

```bash
git add src/viewport.cpp
git commit -m "Feature: entrance direction arrow on road stop placement preview"
```

---

## Summary

| Task | Files | What it does |
|------|-------|-------------|
| 1 | road_gui.h, road_gui.cpp | Expand preview info struct with dry-run params |
| 2 | viewport.cpp | Green/red tint from CMD_BUILD_ROAD_STOP dry-run |
| 3 | viewport.cpp | Arrow sprites showing entrance direction |

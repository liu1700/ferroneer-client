# Road Stop Building Preview — Validation + Direction Arrow

**Date:** 2026-03-29
**Scope:** Truck station and bus station placement preview only
**Branch:** TBD (off master)

---

## Problem

When placing road stops, the player has no hover-time feedback about:
1. Whether the tile is buildable (they must click and read the error message)
2. Which direction vehicles will enter/exit

## Solution

Add two visual cues during road stop placement hover:

1. **Green/red tint** on the building preview — green if buildable, red if not
2. **Arrow sprite** on the station tile showing entrance direction

## Validation Logic

On each frame while the road stop picker is active and the cursor is over a tile:

1. Gather the current placement parameters from `GetRoadStopPlacementPreview()`: station type (bus/truck), orientation (0-5), road type
2. Execute `CMD_BUILD_ROAD_STOP` as a **dry-run** (without `DC_EXEC` / `DoCommandFlag::Execute`) against the cursor tile
3. If the command succeeds: use `PALETTE_TO_STRUCT_BLUE` (renders as green tint via GPU path)
4. If the command fails: use `PALETTE_TO_STRUCT_RED` (renders as red tint via GPU path)

This replaces the current logic that always uses `PALETTE_TO_STRUCT_BLUE` (always green) and only checks `_thd.make_square_red` (which is almost never set during placement hover).

### Performance

The dry-run executes once per frame for the single tile under the cursor. `CMD_BUILD_ROAD_STOP` validation checks tile ownership, slope, water, existing structures, road connectivity — all fast in-memory lookups. No performance concern.

### Accuracy

Using the actual command's dry-run guarantees the preview color matches the build result exactly. Any condition that would cause the build to fail (wrong terrain, no road connection, NewGRF restriction, town authority refusal) will show red.

## Arrow Direction Indicator

### Sprite Selection

Use existing OpenTTD arrow sprites (`src/table/sprites.h:77-80`):
- `SPR_ARROW_DOWN` (SPR_OPENTTD_BASE + 45)
- `SPR_ARROW_UP` (SPR_OPENTTD_BASE + 46)
- `SPR_ARROW_LEFT` (SPR_OPENTTD_BASE + 47)
- `SPR_ARROW_RIGHT` (SPR_OPENTTD_BASE + 48)

These are small 2D UI arrows. They may not look perfectly isometric on the map, but they're functional and require no new art assets. Can be replaced with custom sprites later if needed.

### Orientation-to-Arrow Mapping

Road stop orientations (from `GetRoadStopPlacementPreview().orientation`):

| Orientation | Type | Entry Direction | Arrow(s) |
|-------------|------|----------------|----------|
| 0 | Bay NE | Vehicles enter from NE | SPR_ARROW_RIGHT |
| 1 | Bay SE | Vehicles enter from SE | SPR_ARROW_DOWN |
| 2 | Bay SW | Vehicles enter from SW | SPR_ARROW_LEFT |
| 3 | Bay NW | Vehicles enter from NW | SPR_ARROW_UP |
| 4 | Drive-through NE-SW | Vehicles pass through NE↔SW | SPR_ARROW_RIGHT + SPR_ARROW_LEFT |
| 5 | Drive-through NW-SE | Vehicles pass through NW↔SE | SPR_ARROW_UP + SPR_ARROW_DOWN |

Note: The exact orientation-to-direction mapping needs verification against the actual station graphics. The mapping above is a best guess based on OpenTTD's coordinate system (X increases toward lower-right, Y increases toward lower-left). Verify during implementation by comparing arrow direction with the station's visual entrance.

### Drawing

Arrows are added via `AddTileSpriteToDraw` in the same `DrawTileSelection` block that draws the building preview sprites. They use the same green/red `preview_pal` as the building sprites so the arrow color matches the validation state. The arrow is drawn at the tile's ground level (`ti->x, ti->y, ti->z`).

## Files to Modify

| File | Change |
|------|--------|
| `src/viewport.cpp` | Modify `DrawTileSelection` preview block: add dry-run validation + arrow drawing |
| `src/road_gui.cpp` | May need to expose additional placement parameters (road type, DiagDirection) from the picker window for the dry-run call |

## What This Does NOT Include

- Other building types (rail stations, depots, factories)
- Custom arrow sprites for isometric view
- Changes to the GPU rendering pipeline (existing green/red tint already works after the bit-mask fix)
- Multiplayer authority validation (dry-run uses local player context)

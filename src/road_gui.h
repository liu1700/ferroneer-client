/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file road_gui.h Functions/types related to the road GUIs. */

#ifndef ROAD_GUI_H
#define ROAD_GUI_H

#include "road.h"
#include "road_type.h"
#include "tile_type.h"
#include "direction_type.h"
#include "dropdown_type.h"
#include "newgrf_roadstop.h"

struct Window *ShowBuildRoadToolbar(RoadType roadtype);
struct Window *ShowBuildRoadScenToolbar(RoadType roadtype);
void ConnectRoadToStructure(TileIndex tile, DiagDirection direction);
DropDownList GetRoadTypeDropDownList(RoadTramTypes rtts, bool for_replacement = false, bool all_option = false);
DropDownList GetScenRoadTypeDropDownList(RoadTramTypes rtts);
void InitializeRoadGUI();

/** Info needed to draw a road stop placement preview. */
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
RoadStopPreviewInfo GetRoadStopPlacementPreview();

/** Info needed to draw a road depot placement preview. */
struct RoadDepotPreviewInfo {
	bool active;             ///< Whether the depot picker window is open.
	DiagDirection ddir;      ///< Depot entrance direction.
	RoadType road_type;      ///< Current road type being built.
};
RoadDepotPreviewInfo GetRoadDepotPlacementPreview();

#endif /* ROAD_GUI_H */

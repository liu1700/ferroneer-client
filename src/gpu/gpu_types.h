/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file gpu_types.h Shared types for GPU rendering. */

#ifndef GPU_TYPES_H
#define GPU_TYPES_H

#include <cstdint>

/** Reference to a sprite's location within a texture atlas. */
struct AtlasEntry {
	uint16_t page;      ///< Atlas texture page index.
	float u0, v0;       ///< Top-left UV coordinate in atlas.
	float u1, v1;       ///< Bottom-right UV coordinate in atlas.
	uint16_t width;     ///< Sprite width in pixels.
	uint16_t height;    ///< Sprite height in pixels.
	int16_t x_offs;     ///< Sprite X offset for drawing.
	int16_t y_offs;     ///< Sprite Y offset for drawing.
	bool valid = false; ///< True if this entry has been uploaded to the atlas.
};

#endif /* GPU_TYPES_H */

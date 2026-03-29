/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file remap_table.h GPU texture for palette remap lookups. */

#ifndef GPU_REMAP_TABLE_H
#define GPU_REMAP_TABLE_H

#ifdef WITH_WGPU

#include "wgpu_device.h"
#include "../gfx_type.h"
#include <cstdint>
#include <unordered_map>

/**
 * Builds a 256xN RGBA texture where each row is a palette remap.
 * Row 0 = identity mapping (no remap).
 * Rows 1..N are assigned by scanning all valid recolour sprites and
 * assigning compact indices, so SpriteIDs like 775 (company colours)
 * get a proper row rather than being truncated to 8 bits.
 * Column = original palette index (M-channel value).
 * Texel = RGBA color after applying the remap through the palette.
 */
class RemapTable {
public:
	static constexpr int TABLE_COUNT = 256;

	bool Build();
	WGPUTexture GetTexture() const { return this->texture; }
	int GetRowCount() const { return this->row_count; }
	void Shutdown();

	/** Map a recolour SpriteID to a compact row index in the remap texture.
	 * Returns 0 (identity row) if the sprite is not a valid recolour sprite. */
	uint8_t GetRowIndex(SpriteID sprite_id) const;

private:
	WGPUTexture texture = nullptr;
	int row_count = 0;
	std::unordered_map<SpriteID, uint8_t> sprite_to_row; ///< SpriteID -> compact row index.
};

/** Global remap table pointer.  nullptr when wgpu rendering is not active. */
extern RemapTable *_remap_table;

#endif /* WITH_WGPU */
#endif /* GPU_REMAP_TABLE_H */

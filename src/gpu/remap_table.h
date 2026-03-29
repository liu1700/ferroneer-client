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
#include <cstdint>

/**
 * Builds a 256x256 RGBA texture where each row is a palette remap.
 * Row index = remap table index (from PaletteID & 0xFF).
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

private:
	WGPUTexture texture = nullptr;
	int row_count = 0;
};

#endif /* WITH_WGPU */
#endif /* GPU_REMAP_TABLE_H */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file sprite_atlas.h GPU texture atlas for sprites. */

#ifndef GPU_SPRITE_ATLAS_H
#define GPU_SPRITE_ATLAS_H

#ifdef WITH_WGPU

#include "gpu_types.h"
#include "wgpu_device.h"
#include "../zoom_type.h"
#include "../gfx_type.h"

#include <string>
#include <unordered_map>
#include <vector>

static constexpr int ATLAS_SIZE = 2048;

/** A single atlas page backed by two GPU textures (RGBA + M-channel). */
struct AtlasPage {
	WGPUTexture rgba_texture = nullptr; ///< RGBA colour texture.
	WGPUTexture m_texture    = nullptr; ///< M-channel (recolour mapping) texture.

	/* Shelf packing state. */
	int shelf_y      = 0; ///< Y position of the current shelf top.
	int shelf_height = 0; ///< Height of the tallest sprite on the current shelf.
	int cursor_x     = 0; ///< X cursor within the current shelf.
};

/**
 * Packs game sprites into 2048×2048 GPU texture atlases using shelf packing.
 *
 * Sprites are placed left-to-right on a "shelf" (horizontal row).  When a
 * sprite doesn't fit on the current shelf a new shelf is started below it.
 * When the page is full a new page is allocated.
 */
class SpriteAtlas {
public:
	SpriteAtlas();
	~SpriteAtlas();

	/** Release all GPU textures and forget packed entries. */
	void Reset();

	/**
	 * Upload a decoded sprite to the atlas.
	 * @param id     Sprite ID.
	 * @param data   RGBA pixel data (width * height * 4 bytes).
	 * @param m_data M-channel data (width * height bytes), or nullptr.
	 * @param width  Sprite width in pixels.
	 * @param height Sprite height in pixels.
	 * @param x_offs Sprite X draw offset.
	 * @param y_offs Sprite Y draw offset.
	 * @param zoom   Zoom level of this sprite variant.
	 * @return Atlas entry with UV coordinates, or an invalid entry on failure.
	 */
	AtlasEntry Upload(SpriteID id, const uint8_t *data, const uint8_t *m_data,
		uint16_t width, uint16_t height, int16_t x_offs, int16_t y_offs, ZoomLevel zoom);

	/**
	 * Look up a previously uploaded sprite.
	 * @param id   Sprite ID.
	 * @param zoom Zoom level.
	 * @return Atlas entry, or entry with valid=false if not found.
	 */
	AtlasEntry Get(SpriteID id, ZoomLevel zoom) const;

	/**
	 * Evict a sprite from the atlas (remove from lookup table).
	 * Note: GPU texture memory is not reclaimed — shelf packing does not
	 * support free-space reuse.
	 * @param id Sprite ID.
	 */
	void Evict(SpriteID id);

	/** @return Number of atlas pages currently allocated. */
	size_t GetPageCount() const { return this->pages.size(); }

	/** @return The RGBA texture for the given page, or nullptr. */
	WGPUTexture GetRGBATexture(uint16_t page) const
	{
		if (page >= this->pages.size()) return nullptr;
		return this->pages[page].rgba_texture;
	}

	/** @return The M-channel texture for the given page, or nullptr. */
	WGPUTexture GetMTexture(uint16_t page) const
	{
		if (page >= this->pages.size()) return nullptr;
		return this->pages[page].m_texture;
	}

	/**
	 * Log atlas statistics (PNG dump not yet implemented).
	 * @param directory Ignored for now; reserved for future PNG output path.
	 */
	void DumpToPNG(const std::string &directory) const;

private:
	std::vector<AtlasPage> pages;

	/* Key: (SpriteID << 8) | ZoomLevel */
	using AtlasKey = uint64_t;

	static AtlasKey MakeKey(SpriteID id, ZoomLevel zoom)
	{
		return (static_cast<uint64_t>(id) << 8) | static_cast<uint64_t>(zoom);
	}

	std::unordered_map<AtlasKey, AtlasEntry> entries;

	/** Allocate a new GPU page and append it to pages[]. */
	uint16_t AllocatePage();

	/**
	 * Find a free region of at least (width × height) pixels.
	 * @return Tuple of (page_index, x, y).  page_index == UINT16_MAX on failure.
	 */
	std::tuple<uint16_t, int, int> FindSpace(uint16_t width, uint16_t height);
};

/** Global atlas instance.  nullptr when wgpu rendering is not active. */
extern SpriteAtlas *_sprite_atlas;

#endif /* WITH_WGPU */

#endif /* GPU_SPRITE_ATLAS_H */

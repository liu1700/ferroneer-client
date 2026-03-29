/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file remap_table.cpp Builds a GPU texture encoding palette remap lookups. */

#ifdef WITH_WGPU

#include "../stdafx.h"
#include "remap_table.h"
#include "wgpu_device.h"
#include "../spritecache.h"
#include "../palette_func.h"
#include "../debug.h"

#include "../safeguards.h"

RemapTable *_remap_table = nullptr;

/**
 * Build a 256xN RGBA8 texture where each row is a palette remap table.
 *
 * Row 0 = identity mapping: texel[i] = palette[i].
 * Rows 1..N are populated by scanning all valid recolour sprites in the
 * sprite cache and assigning each a compact row index.  This avoids the
 * old bug where SpriteIDs like 775 (company colours) were truncated to
 * 8 bits (775 & 0xFF = 7), causing a lookup into the wrong row.
 *
 * The SpriteID-to-row mapping is stored in sprite_to_row and queried via
 * GetRowIndex() at draw time.
 */
bool RemapTable::Build()
{
	if (_wgpu_device == nullptr || !_wgpu_device->IsReady()) return false;

	WGPUDevice device = _wgpu_device->GetDevice();
	WGPUQueue queue = _wgpu_device->GetQueue();

	/* 256 columns x TABLE_COUNT rows x 4 bytes (RGBA). */
	static constexpr int WIDTH = 256;
	static constexpr int HEIGHT = TABLE_COUNT;
	std::vector<uint8_t> pixels(WIDTH * HEIGHT * 4, 0);

	/* Helper: write one row of 256 RGBA pixels from palette + optional remap. */
	auto write_row = [&](int row, const uint8_t *remap) {
		uint8_t *dst = pixels.data() + row * WIDTH * 4;
		for (int i = 0; i < 256; i++) {
			uint8_t mapped = (remap != nullptr) ? remap[i] : static_cast<uint8_t>(i);
			const Colour &c = _cur_palette.palette[mapped];
			dst[i * 4 + 0] = c.r;
			dst[i * 4 + 1] = c.g;
			dst[i * 4 + 2] = c.b;
			/* Palette index 0 is always fully transparent. */
			dst[i * 4 + 3] = (mapped == 0) ? 0 : 255;
		}
	};

	/* Row 0: identity mapping (no remap). */
	write_row(0, nullptr);

	/* Scan all sprites for valid recolour sprites and assign compact row
	 * indices.  In practice there are only ~50-100 recolour sprites
	 * (company colours 775-790, special effects like PALETTE_TO_TRANSPARENT
	 * = 802, etc), so they fit easily in 255 rows. */
	this->sprite_to_row.clear();
	int next_row = 1;
	SpriteID max_sprite = GetMaxSpriteID();

	for (SpriteID id = 1; id < max_sprite && next_row < TABLE_COUNT; id++) {
		/* GetNonSprite returns the raw 257-byte recolour sprite data.
		 * The first byte is a type indicator; the remaining 256 bytes are
		 * the remap table: remap[old_index] = new_index. */
		const uint8_t *raw = GetNonSprite(id, SpriteType::Recolour);
		if (raw == nullptr) continue;

		const uint8_t *remap = raw + 1; /* skip 1-byte header */
		write_row(next_row, remap);
		this->sprite_to_row[id] = next_row;
		next_row++;
	}

	this->row_count = next_row;

	/* Create GPU texture.  Always TABLE_COUNT (256) rows so the shader can
	 * use a fixed `/ 256.0` divisor for the V coordinate.  Unused rows
	 * remain zeroed (transparent black), which is harmless. */
	WGPUTextureDescriptor tex_desc{};
	tex_desc.nextInChain = nullptr;
	tex_desc.label = {.data = "remap_table", .length = WGPU_STRLEN};
	tex_desc.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
	tex_desc.dimension = WGPUTextureDimension_2D;
	tex_desc.size = {WIDTH, HEIGHT, 1};
	tex_desc.format = WGPUTextureFormat_RGBA8Unorm;
	tex_desc.mipLevelCount = 1;
	tex_desc.sampleCount = 1;
	tex_desc.viewFormatCount = 0;
	tex_desc.viewFormats = nullptr;
	this->texture = wgpuDeviceCreateTexture(device, &tex_desc);

	if (this->texture == nullptr) {
		Debug(driver, 0, "[remap_table] Failed to create texture");
		return false;
	}

	/* Upload pixel data. */
	WGPUTexelCopyTextureInfo dst_info{};
	dst_info.texture = this->texture;
	dst_info.mipLevel = 0;
	dst_info.origin = {0, 0, 0};
	dst_info.aspect = WGPUTextureAspect_All;

	WGPUTexelCopyBufferLayout layout{};
	layout.offset = 0;
	layout.bytesPerRow = WIDTH * 4;
	layout.rowsPerImage = HEIGHT;

	WGPUExtent3D extent = {WIDTH, HEIGHT, 1};

	wgpuQueueWriteTexture(queue, &dst_info, pixels.data(), pixels.size(), &layout, &extent);

	Debug(driver, 0, "[remap_table] Built {}x{} remap texture ({} recolour sprites mapped)",
		WIDTH, HEIGHT, this->sprite_to_row.size());
	return true;
}

/**
 * Look up the compact row index for a given recolour SpriteID.
 * Returns 0 (identity/no-remap row) if the sprite is unknown.
 */
uint8_t RemapTable::GetRowIndex(SpriteID sprite_id) const
{
	auto it = this->sprite_to_row.find(sprite_id);
	return (it != this->sprite_to_row.end()) ? it->second : 0;
}

void RemapTable::Shutdown()
{
	if (this->texture != nullptr) {
		wgpuTextureRelease(this->texture);
		this->texture = nullptr;
	}
	this->row_count = 0;
	this->sprite_to_row.clear();
}

#endif /* WITH_WGPU */

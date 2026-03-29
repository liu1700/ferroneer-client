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

/**
 * Build a 256x256 RGBA8 texture where each row is a palette remap table.
 *
 * Row 0 = identity mapping: texel[i] = palette[i].
 * Row N (N > 0) = try to load recolour sprite N.  If it exists the 256-byte
 *   remap table is applied: texel[i] = palette[remap[i]].
 *   If not found, the row falls back to the identity mapping.
 *
 * The texture is uploaded once and used by the sprite shader to perform
 * company-colour recolouring entirely on the GPU.
 */
bool RemapTable::Build()
{
	if (_wgpu_device == nullptr || !_wgpu_device->IsReady()) return false;

	WGPUDevice device = _wgpu_device->GetDevice();
	WGPUQueue queue = _wgpu_device->GetQueue();

	/* 256 columns x 256 rows x 4 bytes (RGBA). */
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

	/* Rows 1-255: try to load recolour sprite for each index.
	 * Recolour sprites that exist in the base set include company colours
	 * (775-790), special effects (PALETTE_TO_TRANSPARENT = 802, etc).
	 * Most indices will not correspond to a valid recolour sprite, so we
	 * silently fall back to the identity mapping for those. */
	SpriteID max_sprite = GetMaxSpriteID();
	for (int row = 1; row < TABLE_COUNT; row++) {
		SpriteID sid = static_cast<SpriteID>(row);
		if (sid >= max_sprite) {
			write_row(row, nullptr);
			continue;
		}

		/* GetNonSprite returns the raw 257-byte recolour sprite data.
		 * The first byte is a type indicator; the remaining 256 bytes are
		 * the remap table: remap[old_index] = new_index. */
		const uint8_t *raw = GetNonSprite(sid, SpriteType::Recolour);
		if (raw != nullptr) {
			const uint8_t *remap = raw + 1; /* skip 1-byte header */
			write_row(row, remap);
		} else {
			write_row(row, nullptr);
		}
	}

	/* Create GPU texture. */
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

	this->row_count = TABLE_COUNT;

	Debug(driver, 0, "[remap_table] Built {}x{} remap texture ({} rows)", WIDTH, HEIGHT, TABLE_COUNT);
	return true;
}

void RemapTable::Shutdown()
{
	if (this->texture != nullptr) {
		wgpuTextureRelease(this->texture);
		this->texture = nullptr;
	}
	this->row_count = 0;
}

#endif /* WITH_WGPU */

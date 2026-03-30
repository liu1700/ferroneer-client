/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file sprite_atlas.cpp GPU texture atlas packing and upload. */

#ifdef WITH_WGPU

#include "../stdafx.h"
#include "sprite_atlas.h"
#include "../debug.h"

#include <webgpu/webgpu.h>
#include <algorithm>
#include <cstring>

#include "../safeguards.h"

SpriteAtlas *_sprite_atlas = nullptr;

SpriteAtlas::SpriteAtlas() = default;

SpriteAtlas::~SpriteAtlas()
{
	this->Reset();
}

void SpriteAtlas::Reset()
{
	for (auto &page : this->pages) {
		if (page.rgba_texture != nullptr) {
			wgpuTextureRelease(page.rgba_texture);
			page.rgba_texture = nullptr;
		}
		if (page.m_texture != nullptr) {
			wgpuTextureRelease(page.m_texture);
			page.m_texture = nullptr;
		}
	}

	this->pages.clear();
	this->entries.clear();
}

uint16_t SpriteAtlas::AllocatePage()
{
	if (_wgpu_device == nullptr || !_wgpu_device->IsReady()) return UINT16_MAX;

	WGPUDevice device = _wgpu_device->GetDevice();

	WGPUTextureDescriptor tex_desc{};
	tex_desc.nextInChain   = nullptr;
	tex_desc.size          = {static_cast<uint32_t>(ATLAS_SIZE), static_cast<uint32_t>(ATLAS_SIZE), 1};
	tex_desc.mipLevelCount = 1;
	tex_desc.sampleCount   = 1;
	tex_desc.dimension     = WGPUTextureDimension_2D;
	tex_desc.usage         = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;

	AtlasPage page{};

	/* RGBA colour texture. */
	tex_desc.format = WGPUTextureFormat_RGBA8Unorm;
	tex_desc.label  = {.data = "atlas_rgba", .length = WGPU_STRLEN};
	page.rgba_texture = wgpuDeviceCreateTexture(device, &tex_desc);
	if (page.rgba_texture == nullptr) {
		Debug(sprite, 0, "atlas: failed to create RGBA texture for page {}", this->pages.size());
		return UINT16_MAX;
	}

	/* M-channel (single-channel recolour) texture. */
	tex_desc.format = WGPUTextureFormat_R8Unorm;
	tex_desc.label  = {.data = "atlas_m", .length = WGPU_STRLEN};
	page.m_texture = wgpuDeviceCreateTexture(device, &tex_desc);
	if (page.m_texture == nullptr) {
		wgpuTextureRelease(page.rgba_texture);
		Debug(sprite, 0, "atlas: failed to create M texture for page {}", this->pages.size());
		return UINT16_MAX;
	}

	uint16_t page_idx = static_cast<uint16_t>(this->pages.size());
	this->pages.push_back(page);

	Debug(sprite, 1, "atlas: allocated page {} ({}×{} RGBA + M)", page_idx, ATLAS_SIZE, ATLAS_SIZE);
	return page_idx;
}

std::tuple<uint16_t, int, int> SpriteAtlas::FindSpace(uint16_t width, uint16_t height)
{
	/* Sprites larger than the atlas page cannot be packed. */
	if (width > ATLAS_SIZE || height > ATLAS_SIZE) return {UINT16_MAX, 0, 0};

	for (uint16_t i = 0; i < static_cast<uint16_t>(this->pages.size()); i++) {
		auto &page = this->pages[i];

		/* Fits on the current shelf? */
		if (page.cursor_x + width <= ATLAS_SIZE &&
			page.shelf_y + std::max(page.shelf_height, static_cast<int>(height)) <= ATLAS_SIZE) {
			int x = page.cursor_x;
			int y = page.shelf_y;
			page.cursor_x    += width;
			page.shelf_height = std::max(page.shelf_height, static_cast<int>(height));
			return {i, x, y};
		}

		/* Start a new shelf on this page? */
		int new_shelf_y = page.shelf_y + page.shelf_height;
		if (width <= ATLAS_SIZE && new_shelf_y + height <= ATLAS_SIZE) {
			page.shelf_y      = new_shelf_y;
			page.shelf_height = height;
			page.cursor_x     = width;
			return {i, 0, new_shelf_y};
		}
	}

	/* No existing page has room — allocate a fresh one. */
	uint16_t new_page = this->AllocatePage();
	if (new_page == UINT16_MAX) return {UINT16_MAX, 0, 0};

	auto &page        = this->pages[new_page];
	page.cursor_x     = width;
	page.shelf_height = height;
	/* shelf_y stays 0 */
	return {new_page, 0, 0};
}

AtlasEntry SpriteAtlas::Upload(SpriteID id, const uint8_t *data, const uint8_t *m_data,
	uint16_t width, uint16_t height, int16_t x_offs, int16_t y_offs, ZoomLevel zoom)
{
	AtlasEntry entry{};
	if (width == 0 || height == 0 || data == nullptr) return entry;
	if (_wgpu_device == nullptr || !_wgpu_device->IsReady()) return entry;

	/* Add a 1-pixel duplicated border around every sprite. This gives atlas
	 * sampling a safety margin at sprite edges and avoids leaking neighbouring
	 * sprite texels when coordinates land slightly outside the intended rect. */
	constexpr uint16_t gutter = 1;
	const uint16_t packed_width = width + gutter * 2;
	const uint16_t packed_height = height + gutter * 2;

	AtlasKey key = MakeKey(id, zoom);
	auto existing = this->entries.find(key);
	if (existing != this->entries.end()) {
		static uint32_t reuse_logs = 0;
		if (existing->second.width == width && existing->second.height == height &&
				existing->second.x_offs == x_offs && existing->second.y_offs == y_offs) {
			return existing->second;
		}

		if (reuse_logs < 64) {
			Debug(driver, 0,
				"[gpu][atlas] replacing stale entry for sprite={} zoom={} old_size={}x{} old_offs=({}, {}) new_size={}x{} new_offs=({}, {})",
				id, static_cast<int>(zoom),
				existing->second.width, existing->second.height, existing->second.x_offs, existing->second.y_offs,
				width, height, x_offs, y_offs);
			reuse_logs++;
		}
		this->entries.erase(existing);
	}

	auto [page_idx, x, y] = this->FindSpace(packed_width, packed_height);
	if (page_idx == UINT16_MAX) {
		static uint32_t alloc_fail_logs = 0;
		if (alloc_fail_logs < 64) {
			Debug(driver, 0,
				"[gpu][atlas] upload failed sprite={} zoom={} size={}x{} pages={} entries={}",
				id, static_cast<int>(zoom), width, height, this->pages.size(), this->entries.size());
			alloc_fail_logs++;
		}
		return entry;
	}

	WGPUQueue queue = _wgpu_device->GetQueue();
	auto &page      = this->pages[page_idx];

	auto align_up = [](uint32_t value, uint32_t alignment) -> uint32_t {
		return (value + alignment - 1) & ~(alignment - 1);
	};

	auto clamp_pixel = [](int value, int limit) -> int {
		return std::clamp(value, 0, limit - 1);
	};

	std::vector<uint8_t> rgba_with_gutter(static_cast<size_t>(packed_width) * packed_height * 4);
	for (uint16_t py = 0; py < packed_height; ++py) {
		const int src_y = clamp_pixel(static_cast<int>(py) - gutter, height);
		for (uint16_t px = 0; px < packed_width; ++px) {
			const int src_x = clamp_pixel(static_cast<int>(px) - gutter, width);
			const size_t src_idx = (static_cast<size_t>(src_y) * width + src_x) * 4;
			const size_t dst_idx = (static_cast<size_t>(py) * packed_width + px) * 4;
			std::copy_n(data + src_idx, 4, rgba_with_gutter.data() + dst_idx);
		}
	}

	std::vector<uint8_t> m_with_gutter;
	if (m_data != nullptr) {
		m_with_gutter.resize(static_cast<size_t>(packed_width) * packed_height);
		for (uint16_t py = 0; py < packed_height; ++py) {
			const int src_y = clamp_pixel(static_cast<int>(py) - gutter, height);
			for (uint16_t px = 0; px < packed_width; ++px) {
				const int src_x = clamp_pixel(static_cast<int>(px) - gutter, width);
				const size_t src_idx = static_cast<size_t>(src_y) * width + src_x;
				const size_t dst_idx = static_cast<size_t>(py) * packed_width + px;
				m_with_gutter[dst_idx] = m_data[src_idx];
			}
		}
	}

	/* Upload RGBA data. */
	WGPUTexelCopyTextureInfo dst_rgba{};
	dst_rgba.texture  = page.rgba_texture;
	dst_rgba.mipLevel = 0;
	dst_rgba.origin   = {static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0};
	dst_rgba.aspect   = WGPUTextureAspect_All;

	uint32_t rgba_row_bytes = static_cast<uint32_t>(packed_width) * 4;
	uint32_t rgba_bytes_per_row = align_up(rgba_row_bytes, 256);
	const uint8_t *rgba_upload = rgba_with_gutter.data();
	std::vector<uint8_t> rgba_padded;
	if (rgba_bytes_per_row != rgba_row_bytes) {
		rgba_padded.assign(static_cast<size_t>(rgba_bytes_per_row) * packed_height, 0);
		for (uint16_t row = 0; row < packed_height; row++) {
			std::copy_n(
				rgba_with_gutter.data() + static_cast<size_t>(row) * rgba_row_bytes,
				rgba_row_bytes,
				rgba_padded.data() + static_cast<size_t>(row) * rgba_bytes_per_row
			);
		}
		rgba_upload = rgba_padded.data();
	}

	WGPUTexelCopyBufferLayout rgba_layout{};
	rgba_layout.offset       = 0;
	rgba_layout.bytesPerRow  = rgba_bytes_per_row;
	rgba_layout.rowsPerImage = packed_height;

	WGPUExtent3D extent = {packed_width, packed_height, 1};
	wgpuQueueWriteTexture(queue, &dst_rgba, rgba_upload,
		static_cast<size_t>(rgba_bytes_per_row) * packed_height, &rgba_layout, &extent);

	/* Upload M-channel data if provided. */
	if (m_data != nullptr) {
		WGPUTexelCopyTextureInfo dst_m{};
		dst_m.texture  = page.m_texture;
		dst_m.mipLevel = 0;
		dst_m.origin   = {static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0};
		dst_m.aspect   = WGPUTextureAspect_All;

		uint32_t m_row_bytes = packed_width;
		uint32_t m_bytes_per_row = align_up(m_row_bytes, 256);
		const uint8_t *m_upload = m_with_gutter.data();
		std::vector<uint8_t> m_padded;
		if (m_bytes_per_row != m_row_bytes) {
			m_padded.assign(static_cast<size_t>(m_bytes_per_row) * packed_height, 0);
			for (uint16_t row = 0; row < packed_height; row++) {
				std::copy_n(
					m_with_gutter.data() + static_cast<size_t>(row) * m_row_bytes,
					m_row_bytes,
					m_padded.data() + static_cast<size_t>(row) * m_bytes_per_row
				);
			}
			m_upload = m_padded.data();
		}

		WGPUTexelCopyBufferLayout m_layout{};
		m_layout.offset       = 0;
		m_layout.bytesPerRow  = m_bytes_per_row;
		m_layout.rowsPerImage = packed_height;

		wgpuQueueWriteTexture(queue, &dst_m, m_upload,
			static_cast<size_t>(m_bytes_per_row) * packed_height, &m_layout, &extent);
	}

	/* Compute normalised UV coordinates for the inner sprite rect. Using texel
	 * centres avoids edge ambiguity with nearest sampling. */
	const float inv = 1.0f / static_cast<float>(ATLAS_SIZE);
	entry.page   = page_idx;
	entry.u0     = static_cast<float>(x + gutter) + 0.5f;
	entry.v0     = static_cast<float>(y + gutter) + 0.5f;
	entry.u1     = static_cast<float>(x + gutter + width) - 0.5f;
	entry.v1     = static_cast<float>(y + gutter + height) - 0.5f;
	entry.u0    *= inv;
	entry.v0    *= inv;
	entry.u1    *= inv;
	entry.v1    *= inv;
	entry.width  = width;
	entry.height = height;
	entry.x_offs = x_offs;
	entry.y_offs = y_offs;
	entry.valid  = true;

	this->entries[key] = entry;

	static uint32_t upload_logs = 0;
	if (upload_logs < 128) {
		Debug(driver, 1,
			"[gpu][atlas] upload sprite={} zoom={} page={} pos=({}, {}) size={}x{} offs=({}, {}) pages={} entries={}",
			id, static_cast<int>(zoom), page_idx, x, y, width, height, x_offs, y_offs,
			this->pages.size(), this->entries.size());
		upload_logs++;
	}

	return entry;
}

AtlasEntry SpriteAtlas::Get(SpriteID id, ZoomLevel zoom) const
{
	auto it = this->entries.find(MakeKey(id, zoom));
	if (it != this->entries.end()) return it->second;

	static uint32_t miss_logs = 0;
	if (miss_logs < 128) {
		Debug(driver, 1, "[gpu][atlas] miss sprite={} zoom={} pages={} entries={}",
			id, static_cast<int>(zoom), this->pages.size(), this->entries.size());
		miss_logs++;
	}

	return AtlasEntry{};
}

void SpriteAtlas::Evict(SpriteID id)
{
	/* Remove all zoom variants of this sprite from the lookup table.
	 * GPU texture memory is not reclaimed — shelf packing does not support
	 * free-space reuse.  Rebuild the atlas if fragmentation becomes a problem. */
	for (int z = static_cast<int>(ZoomLevel::Begin); z < static_cast<int>(ZoomLevel::End); z++) {
		this->entries.erase(MakeKey(id, static_cast<ZoomLevel>(z)));
	}
}

void SpriteAtlas::DumpToPNG(const std::string & /*directory*/) const
{
	/* PNG read-back requires a GPU download pass and a PNG library.
	 * For now, log statistics that can be inspected from the console. */
	Debug(sprite, 0, "atlas: {} page(s), {} entr(ies)",
		this->pages.size(), this->entries.size());
	for (size_t i = 0; i < this->pages.size(); i++) {
		const auto &page = this->pages[i];
		Debug(sprite, 0, "atlas:   page {} — shelf_y={} shelf_h={} cursor_x={}",
			i, page.shelf_y, page.shelf_height, page.cursor_x);
	}
}

#endif /* WITH_WGPU */

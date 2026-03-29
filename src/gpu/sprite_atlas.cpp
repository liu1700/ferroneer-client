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

	auto [page_idx, x, y] = this->FindSpace(width, height);
	if (page_idx == UINT16_MAX) return entry;

	WGPUQueue queue = _wgpu_device->GetQueue();
	auto &page      = this->pages[page_idx];

	/* Upload RGBA data. */
	WGPUTexelCopyTextureInfo dst_rgba{};
	dst_rgba.texture  = page.rgba_texture;
	dst_rgba.mipLevel = 0;
	dst_rgba.origin   = {static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0};
	dst_rgba.aspect   = WGPUTextureAspect_All;

	WGPUTexelCopyBufferLayout rgba_layout{};
	rgba_layout.offset       = 0;
	rgba_layout.bytesPerRow  = static_cast<uint32_t>(width) * 4;
	rgba_layout.rowsPerImage = height;

	WGPUExtent3D extent = {width, height, 1};
	wgpuQueueWriteTexture(queue, &dst_rgba, data,
		static_cast<size_t>(width) * height * 4, &rgba_layout, &extent);

	/* Upload M-channel data if provided. */
	if (m_data != nullptr) {
		WGPUTexelCopyTextureInfo dst_m{};
		dst_m.texture  = page.m_texture;
		dst_m.mipLevel = 0;
		dst_m.origin   = {static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0};
		dst_m.aspect   = WGPUTextureAspect_All;

		WGPUTexelCopyBufferLayout m_layout{};
		m_layout.offset       = 0;
		m_layout.bytesPerRow  = width;
		m_layout.rowsPerImage = height;

		wgpuQueueWriteTexture(queue, &dst_m, m_data,
			static_cast<size_t>(width) * height, &m_layout, &extent);
	}

	/* Compute normalised UV coordinates. */
	const float inv = 1.0f / static_cast<float>(ATLAS_SIZE);
	entry.page   = page_idx;
	entry.u0     = static_cast<float>(x)         * inv;
	entry.v0     = static_cast<float>(y)         * inv;
	entry.u1     = static_cast<float>(x + width) * inv;
	entry.v1     = static_cast<float>(y + height) * inv;
	entry.width  = width;
	entry.height = height;
	entry.x_offs = x_offs;
	entry.y_offs = y_offs;
	entry.valid  = true;

	this->entries[MakeKey(id, zoom)] = entry;
	return entry;
}

AtlasEntry SpriteAtlas::Get(SpriteID id, ZoomLevel zoom) const
{
	auto it = this->entries.find(MakeKey(id, zoom));
	if (it != this->entries.end()) return it->second;
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

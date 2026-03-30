/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file sprite_command.h GPU sprite draw command buffer. */

#ifndef GPU_SPRITE_COMMAND_H
#define GPU_SPRITE_COMMAND_H

#ifdef WITH_WGPU

#include <cstdint>
#include <vector>
#include <array>

/**
 * Per-instance data uploaded to the GPU for instanced sprite rendering.
 * Layout must match the WGSL shader's SpriteInstance struct.
 * Total size: 52 bytes per instance.
 */
struct GpuSpriteInstance {
	float screen_pos[2];  ///< Screen pixel position (top-left of sprite).
	float size[2];        ///< Sprite width and height in screen pixels.
	float uv_min[2];      ///< Top-left UV in atlas (u0, v0).
	float uv_max[2];      ///< Bottom-right UV in atlas (u1, v1).
	float z_depth;        ///< Depth for GPU depth test. Smaller = closer to camera.
	uint32_t mode_remap;  ///< Packed: mode(8) | remap_index(8) | alpha(8) | unused(8).
	float tint[3];        ///< Per-sprite RGB tint multiplier (1.0 = no tint).
};
static_assert(sizeof(GpuSpriteInstance) == 52);

/** Blend/render modes matching the WGSL shader. */
enum GpuSpriteMode : uint8_t {
	GPU_SPRITE_NORMAL      = 0, ///< Standard RGBA rendering.
	GPU_SPRITE_REMAP       = 1, ///< M-channel company color recoloring.
	GPU_SPRITE_TRANSPARENT = 2, ///< Darkened semi-transparent (like PALETTE_TO_TRANSPARENT).
	GPU_SPRITE_ALPHA_BLEND = 3, ///< Per-sprite alpha blending.
};

/** Pack mode, remap index, and alpha into a single uint32_t. */
inline uint32_t PackModeRemap(uint8_t mode, uint8_t remap_index, uint8_t alpha)
{
	return static_cast<uint32_t>(mode)
	     | (static_cast<uint32_t>(remap_index) << 8)
	     | (static_cast<uint32_t>(alpha) << 16);
}

/**
 * Per-frame buffer of sprite draw commands, organized by atlas page.
 *
 * During viewport rendering, commands are emitted into per-page vectors.
 * At frame end, GpuRenderer iterates each page's batch for one draw call per page.
 */
class SpriteCommandBuffer {
public:
	static constexpr size_t MAX_ATLAS_PAGES = 128;

	/** Clear all batches for a new frame. */
	void Reset();

	/** Emit a sprite instance into the batch for the given atlas page. */
	void Emit(uint16_t atlas_page, const GpuSpriteInstance &instance);

	/** @return Number of instances in a specific page batch. */
	size_t PageCount(uint16_t page) const;

	/** @return Pointer to instance data for a page batch. */
	const GpuSpriteInstance *PageData(uint16_t page) const;

	/** @return Total number of instances across all pages. */
	size_t TotalCount() const;

	/** @return Highest page index that has any instances. */
	uint16_t MaxActivePage() const;

private:
	struct PageBatch {
		std::vector<GpuSpriteInstance> instances;
		void Reset() { instances.clear(); }
	};
	std::array<PageBatch, MAX_ATLAS_PAGES> batches;
};

/** Global command buffer. Non-null when GPU rendering is active. */
extern SpriteCommandBuffer *_gpu_command_buffer;

/** When true, EmitGpuSpriteCommand skips emission (used during DrawDirtyBlocks phase). */
extern bool _gpu_suppress_sprite_emit;

#endif /* WITH_WGPU */

#endif /* GPU_SPRITE_COMMAND_H */

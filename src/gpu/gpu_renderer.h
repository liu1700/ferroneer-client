/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file gpu_renderer.h GPU renderer with instanced sprite pipeline and UI composite. */

#ifndef GPU_RENDERER_H
#define GPU_RENDERER_H

#ifdef WITH_WGPU

#include "wgpu_device.h"
#include "remap_table.h"
#include "sprite_command.h"

#include <vector>

/**
 * Renders instanced sprite quads and composites the CPU-rendered UI buffer.
 *
 * The frame loop is: BeginFrame() -> SubmitSprites() -> CompositeUI() -> Present().
 * Sprite rendering uses a depth buffer for correct overlap ordering.
 * The UI layer is drawn on top with alpha blending.
 */
class GpuRenderer {
public:
	GpuRenderer();
	~GpuRenderer();

	GpuRenderer(const GpuRenderer &) = delete;
	GpuRenderer &operator=(const GpuRenderer &) = delete;

	/** Initialise all GPU resources (pipelines, bind groups, depth buffer, UI). */
	bool Init(WgpuDevice *device);

	/** Recreate size-dependent resources (depth buffer, viewport uniforms, UI texture). */
	void Resize(int width, int height);

	/** Update sprite coordinate mapping using logical screen dimensions. */
	void SetLogicalViewportSize(int width, int height);

	/** Acquire the next surface texture and create the command encoder. */
	bool BeginFrame();

	/** Render all sprite batches from the command buffer. */
	void SubmitSprites(const SpriteCommandBuffer &commands);

	/** Upload the CPU UI buffer and composite it over the sprite layer. */
	void CompositeUI(const void *pixels, int width, int height);

	/** Finish the command encoder, submit, and present. */
	void Present();

	/** Release all GPU resources. */
	void Shutdown();

private:
	WgpuDevice *dev = nullptr; ///< Non-owning pointer to the wgpu device wrapper.

	/* --- Sprite pipeline --- */
	WGPUShaderModule sprite_shader = nullptr;
	WGPURenderPipeline sprite_pipeline = nullptr;
	WGPUBindGroupLayout viewport_bgl = nullptr;   ///< group(0): viewport uniforms.
	WGPUBindGroup viewport_bind_group = nullptr;
	WGPUBuffer viewport_uniform_buf = nullptr;
	WGPUBindGroupLayout atlas_bgl = nullptr;       ///< group(1): atlas textures + sampler.
	WGPUSampler atlas_sampler = nullptr;
	RemapTable remap_table;                        ///< 256x256 RGBA remap lookup texture.
	WGPUSampler remap_sampler = nullptr;           ///< Nearest-filter sampler for remap table.
	bool remap_table_built = false;                ///< Whether the remap table has been built.
	std::vector<WGPUBindGroup> atlas_bind_groups;  ///< One per atlas page, lazily created.
	WGPUBuffer instance_buffer = nullptr;
	size_t instance_buffer_capacity = 0;           ///< In number of GpuSpriteInstance elements.

	/* --- Depth buffer --- */
	WGPUTexture depth_texture = nullptr;
	WGPUTextureView depth_view = nullptr;

	/* --- UI composite pipeline --- */
	WGPUShaderModule ui_shader = nullptr;
	WGPURenderPipeline ui_pipeline = nullptr;
	WGPUBindGroupLayout ui_bgl = nullptr;
	WGPUBindGroup ui_bind_group = nullptr;
	WGPUTexture ui_texture = nullptr;
	WGPUTextureView ui_texture_view = nullptr;
	WGPUSampler ui_sampler = nullptr;
	int ui_tex_width = 0;
	int ui_tex_height = 0;

	/* --- Per-frame state (valid between BeginFrame and Present) --- */
	WGPUSurfaceTexture surface_texture{};
	WGPUTextureView surface_view = nullptr;
	WGPUCommandEncoder encoder = nullptr;
	int frame_width = 0;
	int frame_height = 0;

	/* --- Helper methods --- */
	void CreateDepthBuffer(int w, int h);
	void DestroyDepthBuffer();
	void CreateViewportUniform();
	void UpdateViewportUniform(int w, int h);
	void EnsureInstanceBuffer(size_t required_count);
	void EnsureRemapTable();
	void EnsureAtlasBindGroups(uint16_t page_count);
	void CreateUIResources(int w, int h);
	void DestroyUIResources();
	WGPUShaderModule CreateShaderModule(const char *label, const char *wgsl_code);
};

#endif /* WITH_WGPU */

#endif /* GPU_RENDERER_H */

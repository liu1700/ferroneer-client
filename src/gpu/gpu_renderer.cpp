/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file gpu_renderer.cpp GPU renderer — instanced sprite pipeline + UI composite. */

#ifdef WITH_WGPU

#include "../stdafx.h"
#include "gpu_renderer.h"
#include "sprite_atlas.h"
#include "../debug.h"

#include "../safeguards.h"

/* =========================================================================
 * Embedded WGSL shaders
 * ========================================================================= */

/**
 * Sprite shader: expands vertex_index 0-3 into a quad (triangle strip),
 * transforms screen pixels to NDC using viewport scale/offset, samples the
 * atlas RGBA texture, and applies transparency / tint / alpha effects.
 */
static constexpr const char *kSpriteShaderWGSL = R"(
struct ViewportUniforms {
    scale: vec2f,   /* vec2(2.0/width, -2.0/height) */
    offset: vec2f,  /* vec2(-1.0, 1.0)              */
};
@group(0) @binding(0) var<uniform> viewport: ViewportUniforms;

@group(1) @binding(0) var atlas_rgba: texture_2d<f32>;
@group(1) @binding(1) var atlas_m: texture_2d<f32>;
@group(1) @binding(2) var atlas_sampler: sampler;
@group(1) @binding(3) var remap_table: texture_2d<f32>;
@group(1) @binding(4) var remap_sampler: sampler;

struct SpriteInstance {
    @location(0) screen_pos: vec2f,
    @location(1) size: vec2f,
    @location(2) uv_min: vec2f,
    @location(3) uv_max: vec2f,
    @location(4) z_depth: f32,
    @location(5) mode_remap: u32,
    @location(6) tint: vec3f,
};

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) @interpolate(flat) mode: u32,
    @location(2) @interpolate(flat) remap_index: u32,
    @location(3) @interpolate(flat) alpha: f32,
    @location(4) @interpolate(flat) tint: vec3f,
};

@vertex fn vs_sprite(
    @builtin(vertex_index) vi: u32,
    inst: SpriteInstance,
) -> VertexOutput {
    /* Expand vertex_index 0-3 into quad corners (triangle strip order). */
    let corner = vec2f(
        f32(vi & 1u),         /* 0, 1, 0, 1 */
        f32((vi >> 1u) & 1u), /* 0, 0, 1, 1 */
    );

    /* Screen-space pixel position. */
    let pixel_pos = inst.screen_pos + corner * inst.size;

    /* Convert screen pixels to NDC: x' = x * (2/w) - 1, y' = y * (-2/h) + 1 */
    let ndc = pixel_pos * viewport.scale + viewport.offset;

    var out: VertexOutput;
    out.position = vec4f(ndc, inst.z_depth, 1.0);
    out.uv = mix(inst.uv_min, inst.uv_max, corner);

    /* Unpack mode_remap: mode(8) | remap_index(8) | alpha(8) | unused(8) */
    out.mode = inst.mode_remap & 0xFFu;
    out.remap_index = (inst.mode_remap >> 8u) & 0xFFu;
    out.alpha = f32((inst.mode_remap >> 16u) & 0xFFu) / 255.0;
    out.tint = inst.tint;

    return out;
}

@fragment fn fs_sprite(in: VertexOutput) -> @location(0) vec4f {
    var color = textureSample(atlas_rgba, atlas_sampler, in.uv);

    /* Alpha test — discard fully transparent pixels. */
    if (color.a < 0.01) {
        discard;
    }

    /* Mode 1 (Remap): apply M-channel recoloring via remap table. */
    if (in.mode == 1u) {
        let m = textureSample(atlas_m, atlas_sampler, in.uv).r;
        if (m > 0.001) {
            let remap_uv = vec2f(m, (f32(in.remap_index) + 0.5) / 256.0);
            let remapped = textureSample(remap_table, remap_sampler, remap_uv);
            color = vec4f(remapped.rgb, color.a);
        }
    }

    /* Mode 2: transparent darkening (like PALETTE_TO_TRANSPARENT). */
    if (in.mode == 2u) {
        color = vec4f(color.rgb * 0.4, color.a * 0.6);
    }

    /* Apply tint (1.0 = no change). */
    color = vec4f(color.rgb * in.tint, color.a * in.alpha);

    return color;
}
)";

/**
 * UI composite shader: draws a fullscreen triangle, samples the CPU-uploaded
 * UI texture, and swizzles BGRA -> RGBA.
 */
static constexpr const char *kUICompositeShaderWGSL = R"(
struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_ui(@builtin(vertex_index) vi: u32) -> VertexOutput {
    /* Fullscreen triangle covering clip space. */
    var pos = array<vec2f, 3>(
        vec2f(-1.0, -1.0),
        vec2f( 3.0, -1.0),
        vec2f(-1.0,  3.0),
    );
    var out: VertexOutput;
    out.position = vec4f(pos[vi], 0.0, 1.0);
    /* Map clip space [-1,1] to UV [0,1] with Y flipped for top-left origin. */
    out.uv = pos[vi] * vec2f(0.5, -0.5) + vec2f(0.5, 0.5);
    return out;
}

@group(0) @binding(0) var ui_texture: texture_2d<f32>;
@group(0) @binding(1) var ui_sampler: sampler;

@fragment fn fs_ui(in: VertexOutput) -> @location(0) vec4f {
    /* The UI texture is BGRA8Unorm — the GPU automatically converts to
     * logical RGBA when sampling, so no manual swizzle is needed. */
    return textureSample(ui_texture, ui_sampler, in.uv);
}
)";

/* =========================================================================
 * GpuRenderer implementation
 * ========================================================================= */

GpuRenderer::GpuRenderer() = default;
GpuRenderer::~GpuRenderer() = default;

WGPUShaderModule GpuRenderer::CreateShaderModule(const char *label, const char *wgsl_code)
{
	WGPUShaderSourceWGSL wgsl_src{};
	wgsl_src.chain.next = nullptr;
	wgsl_src.chain.sType = WGPUSType_ShaderSourceWGSL;
	wgsl_src.code = {.data = wgsl_code, .length = WGPU_STRLEN};

	WGPUShaderModuleDescriptor sm_desc{};
	sm_desc.nextInChain = &wgsl_src.chain;
	sm_desc.label = {.data = label, .length = WGPU_STRLEN};

	WGPUShaderModule module = wgpuDeviceCreateShaderModule(this->dev->GetDevice(), &sm_desc);
	Debug(driver, 1, "[gpu_renderer] shader '{}' = {}", label, (void *)module);
	return module;
}

/* -------------------------------------------------------------------------
 * Init — create all persistent GPU resources
 * -------------------------------------------------------------------------*/

bool GpuRenderer::Init(WgpuDevice *device)
{
	this->dev = device;
	WGPUDevice d = this->dev->GetDevice();

	/* --- 1. Compile shader modules --- */
	this->sprite_shader = this->CreateShaderModule("sprite_shader", kSpriteShaderWGSL);
	if (this->sprite_shader == nullptr) return false;

	this->ui_shader = this->CreateShaderModule("ui_composite_shader", kUICompositeShaderWGSL);
	if (this->ui_shader == nullptr) return false;

	/* --- 2. Viewport bind group layout: group(0) = uniform buffer --- */
	{
		WGPUBindGroupLayoutEntry entry{};
		entry.nextInChain = nullptr;
		entry.binding = 0;
		entry.visibility = WGPUShaderStage_Vertex;
		entry.buffer.type = WGPUBufferBindingType_Uniform;
		entry.buffer.hasDynamicOffset = false;
		entry.buffer.minBindingSize = 16; /* vec2f scale + vec2f offset = 16 bytes */
		entry.texture = {};
		entry.sampler = {};
		entry.storageTexture = {};

		WGPUBindGroupLayoutDescriptor bgl_desc{};
		bgl_desc.nextInChain = nullptr;
		bgl_desc.label = {.data = "viewport_bgl", .length = WGPU_STRLEN};
		bgl_desc.entryCount = 1;
		bgl_desc.entries = &entry;
		this->viewport_bgl = wgpuDeviceCreateBindGroupLayout(d, &bgl_desc);
	}

	/* --- 3. Atlas bind group layout: group(1) = rgba, m, sampler, remap_table, remap_sampler --- */
	{
		WGPUBindGroupLayoutEntry entries[5]{};

		/* binding 0 — atlas RGBA texture */
		entries[0].nextInChain = nullptr;
		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Fragment;
		entries[0].texture.sampleType = WGPUTextureSampleType_Float;
		entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[0].texture.multisampled = false;
		entries[0].buffer = {};
		entries[0].sampler = {};
		entries[0].storageTexture = {};

		/* binding 1 — atlas M texture */
		entries[1].nextInChain = nullptr;
		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Fragment;
		entries[1].texture.sampleType = WGPUTextureSampleType_Float;
		entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[1].texture.multisampled = false;
		entries[1].buffer = {};
		entries[1].sampler = {};
		entries[1].storageTexture = {};

		/* binding 2 — atlas sampler */
		entries[2].nextInChain = nullptr;
		entries[2].binding = 2;
		entries[2].visibility = WGPUShaderStage_Fragment;
		entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
		entries[2].buffer = {};
		entries[2].texture = {};
		entries[2].storageTexture = {};

		/* binding 3 — remap table texture */
		entries[3].nextInChain = nullptr;
		entries[3].binding = 3;
		entries[3].visibility = WGPUShaderStage_Fragment;
		entries[3].texture.sampleType = WGPUTextureSampleType_Float;
		entries[3].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[3].texture.multisampled = false;
		entries[3].buffer = {};
		entries[3].sampler = {};
		entries[3].storageTexture = {};

		/* binding 4 — remap sampler (nearest filtering) */
		entries[4].nextInChain = nullptr;
		entries[4].binding = 4;
		entries[4].visibility = WGPUShaderStage_Fragment;
		entries[4].sampler.type = WGPUSamplerBindingType_Filtering;
		entries[4].buffer = {};
		entries[4].texture = {};
		entries[4].storageTexture = {};

		WGPUBindGroupLayoutDescriptor bgl_desc{};
		bgl_desc.nextInChain = nullptr;
		bgl_desc.label = {.data = "atlas_bgl", .length = WGPU_STRLEN};
		bgl_desc.entryCount = 5;
		bgl_desc.entries = entries;
		this->atlas_bgl = wgpuDeviceCreateBindGroupLayout(d, &bgl_desc);
	}

	/* --- 4. Pipeline layout (2 bind groups: viewport + atlas) --- */
	WGPUBindGroupLayout bgl_array[2] = {this->viewport_bgl, this->atlas_bgl};

	WGPUPipelineLayoutDescriptor pl_desc{};
	pl_desc.nextInChain = nullptr;
	pl_desc.label = {.data = "sprite_pipeline_layout", .length = WGPU_STRLEN};
	pl_desc.bindGroupLayoutCount = 2;
	pl_desc.bindGroupLayouts = bgl_array;
	WGPUPipelineLayout sprite_pl = wgpuDeviceCreatePipelineLayout(d, &pl_desc);

	/* --- 5. Vertex attribute layout for GpuSpriteInstance (52 bytes, 7 attributes) --- */
	WGPUVertexAttribute attrs[7]{};
	/* location(0): screen_pos — float32x2, offset 0 */
	attrs[0].format = WGPUVertexFormat_Float32x2;
	attrs[0].offset = 0;
	attrs[0].shaderLocation = 0;
	/* location(1): size — float32x2, offset 8 */
	attrs[1].format = WGPUVertexFormat_Float32x2;
	attrs[1].offset = 8;
	attrs[1].shaderLocation = 1;
	/* location(2): uv_min — float32x2, offset 16 */
	attrs[2].format = WGPUVertexFormat_Float32x2;
	attrs[2].offset = 16;
	attrs[2].shaderLocation = 2;
	/* location(3): uv_max — float32x2, offset 24 */
	attrs[3].format = WGPUVertexFormat_Float32x2;
	attrs[3].offset = 24;
	attrs[3].shaderLocation = 3;
	/* location(4): z_depth — float32, offset 32 */
	attrs[4].format = WGPUVertexFormat_Float32;
	attrs[4].offset = 32;
	attrs[4].shaderLocation = 4;
	/* location(5): mode_remap — uint32, offset 36 */
	attrs[5].format = WGPUVertexFormat_Uint32;
	attrs[5].offset = 36;
	attrs[5].shaderLocation = 5;
	/* location(6): tint — float32x3, offset 40 */
	attrs[6].format = WGPUVertexFormat_Float32x3;
	attrs[6].offset = 40;
	attrs[6].shaderLocation = 6;

	WGPUVertexBufferLayout vbl{};
	vbl.arrayStride = sizeof(GpuSpriteInstance); /* 52 bytes */
	vbl.stepMode = WGPUVertexStepMode_Instance;
	vbl.attributeCount = 7;
	vbl.attributes = attrs;

	/* --- 6. Blend state (premultiplied alpha) --- */
	WGPUBlendState blend{};
	blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
	blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
	blend.color.operation = WGPUBlendOperation_Add;
	blend.alpha.srcFactor = WGPUBlendFactor_One;
	blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
	blend.alpha.operation = WGPUBlendOperation_Add;

	/* --- 7. Color target state --- */
	WGPUColorTargetState color_target{};
	color_target.nextInChain = nullptr;
	color_target.format = this->dev->GetSurfaceFormat();
	color_target.blend = &blend;
	color_target.writeMask = WGPUColorWriteMask_All;

	/* --- 8. Depth stencil state --- */
	WGPUDepthStencilState depth_stencil{};
	depth_stencil.nextInChain = nullptr;
	depth_stencil.format = WGPUTextureFormat_Depth24Plus;
	depth_stencil.depthWriteEnabled = WGPUOptionalBool_True;
	depth_stencil.depthCompare = WGPUCompareFunction_Less;
	depth_stencil.stencilFront.compare = WGPUCompareFunction_Always;
	depth_stencil.stencilFront.failOp = WGPUStencilOperation_Keep;
	depth_stencil.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
	depth_stencil.stencilFront.passOp = WGPUStencilOperation_Keep;
	depth_stencil.stencilBack = depth_stencil.stencilFront;
	depth_stencil.stencilReadMask = 0;
	depth_stencil.stencilWriteMask = 0;
	depth_stencil.depthBias = 0;
	depth_stencil.depthBiasSlopeScale = 0.0f;
	depth_stencil.depthBiasClamp = 0.0f;

	/* --- 9. Fragment state --- */
	WGPUFragmentState frag_state{};
	frag_state.nextInChain = nullptr;
	frag_state.module = this->sprite_shader;
	frag_state.entryPoint = {.data = "fs_sprite", .length = WGPU_STRLEN};
	frag_state.constantCount = 0;
	frag_state.constants = nullptr;
	frag_state.targetCount = 1;
	frag_state.targets = &color_target;

	/* --- 10. Render pipeline --- */
	WGPURenderPipelineDescriptor rp_desc{};
	rp_desc.nextInChain = nullptr;
	rp_desc.label = {.data = "sprite_pipeline", .length = WGPU_STRLEN};
	rp_desc.layout = sprite_pl;

	rp_desc.vertex.nextInChain = nullptr;
	rp_desc.vertex.module = this->sprite_shader;
	rp_desc.vertex.entryPoint = {.data = "vs_sprite", .length = WGPU_STRLEN};
	rp_desc.vertex.constantCount = 0;
	rp_desc.vertex.constants = nullptr;
	rp_desc.vertex.bufferCount = 1;
	rp_desc.vertex.buffers = &vbl;

	rp_desc.primitive.nextInChain = nullptr;
	rp_desc.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
	rp_desc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
	rp_desc.primitive.frontFace = WGPUFrontFace_CCW;
	rp_desc.primitive.cullMode = WGPUCullMode_None;
	rp_desc.primitive.unclippedDepth = false;

	rp_desc.depthStencil = &depth_stencil;

	rp_desc.multisample.nextInChain = nullptr;
	rp_desc.multisample.count = 1;
	rp_desc.multisample.mask = 0xFFFFFFFF;
	rp_desc.multisample.alphaToCoverageEnabled = false;

	rp_desc.fragment = &frag_state;

	this->sprite_pipeline = wgpuDeviceCreateRenderPipeline(d, &rp_desc);
	Debug(driver, 0, "[gpu_renderer] sprite_pipeline = {}", (void *)this->sprite_pipeline);

	/* Release pipeline layout — now owned by the pipeline. */
	wgpuPipelineLayoutRelease(sprite_pl);

	/* --- 11. Atlas sampler (nearest filtering for pixel art) --- */
	{
		WGPUSamplerDescriptor samp_desc{};
		samp_desc.nextInChain = nullptr;
		samp_desc.label = {.data = "atlas_sampler", .length = WGPU_STRLEN};
		samp_desc.addressModeU = WGPUAddressMode_ClampToEdge;
		samp_desc.addressModeV = WGPUAddressMode_ClampToEdge;
		samp_desc.addressModeW = WGPUAddressMode_ClampToEdge;
		samp_desc.magFilter = WGPUFilterMode_Nearest;
		samp_desc.minFilter = WGPUFilterMode_Nearest;
		samp_desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
		samp_desc.lodMinClamp = 0.0f;
		samp_desc.lodMaxClamp = 1.0f;
		samp_desc.compare = WGPUCompareFunction_Undefined;
		samp_desc.maxAnisotropy = 1;
		this->atlas_sampler = wgpuDeviceCreateSampler(d, &samp_desc);
	}

	/* --- 11b. Remap sampler (nearest filtering for exact palette lookups) --- */
	{
		WGPUSamplerDescriptor remap_desc{};
		remap_desc.nextInChain = nullptr;
		remap_desc.label = {.data = "remap_sampler", .length = WGPU_STRLEN};
		remap_desc.addressModeU = WGPUAddressMode_ClampToEdge;
		remap_desc.addressModeV = WGPUAddressMode_ClampToEdge;
		remap_desc.addressModeW = WGPUAddressMode_ClampToEdge;
		remap_desc.magFilter = WGPUFilterMode_Nearest;
		remap_desc.minFilter = WGPUFilterMode_Nearest;
		remap_desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
		remap_desc.lodMinClamp = 0.0f;
		remap_desc.lodMaxClamp = 1.0f;
		remap_desc.compare = WGPUCompareFunction_Undefined;
		remap_desc.maxAnisotropy = 1;
		this->remap_sampler = wgpuDeviceCreateSampler(d, &remap_desc);
	}

	/* --- 12. Viewport uniform buffer + bind group --- */
	this->CreateViewportUniform();

	/* --- 13. Depth buffer and UI resources (size-dependent) --- */
	int w = this->dev->GetWidth();
	int h = this->dev->GetHeight();
	this->CreateDepthBuffer(w, h);
	this->CreateUIResources(w, h);
	this->UpdateViewportUniform(w, h);

	Debug(driver, 0, "[gpu_renderer] Init complete ({}x{})", w, h);
	return true;
}

/* -------------------------------------------------------------------------
 * Resize — recreate size-dependent resources
 * -------------------------------------------------------------------------*/

void GpuRenderer::Resize(int width, int height)
{
	if (this->dev == nullptr) return;

	this->DestroyDepthBuffer();
	this->CreateDepthBuffer(width, height);

	this->DestroyUIResources();
	this->CreateUIResources(width, height);

	this->UpdateViewportUniform(width, height);

	Debug(driver, 1, "[gpu_renderer] Resize to {}x{}", width, height);
}

/* -------------------------------------------------------------------------
 * BeginFrame — acquire surface, create encoder
 * -------------------------------------------------------------------------*/

bool GpuRenderer::BeginFrame()
{
	if (this->dev == nullptr || !this->dev->IsReady()) return false;

	WGPUSurface surface = this->dev->GetSurface();

	this->surface_texture = {};
	wgpuSurfaceGetCurrentTexture(surface, &this->surface_texture);

	switch (this->surface_texture.status) {
		case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
			break;

		case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
			Debug(driver, 1, "[gpu_renderer] Surface suboptimal, reconfiguring");
			this->dev->ReconfigureSurface();
			break;

		case WGPUSurfaceGetCurrentTextureStatus_Timeout:
			Debug(driver, 1, "[gpu_renderer] Surface acquire timed out");
			return false;

		case WGPUSurfaceGetCurrentTextureStatus_Outdated:
		case WGPUSurfaceGetCurrentTextureStatus_Lost:
			Debug(driver, 1, "[gpu_renderer] Surface lost/outdated");
			return false;

		case WGPUSurfaceGetCurrentTextureStatus_OutOfMemory:
		case WGPUSurfaceGetCurrentTextureStatus_DeviceLost:
		case WGPUSurfaceGetCurrentTextureStatus_Error:
		default:
			Debug(driver, 0, "[gpu_renderer] Surface acquire failed, status={}", static_cast<int>(this->surface_texture.status));
			return false;
	}

	this->surface_view = wgpuTextureCreateView(this->surface_texture.texture, nullptr);

	this->frame_width = this->dev->GetWidth();
	this->frame_height = this->dev->GetHeight();

	WGPUCommandEncoderDescriptor enc_desc{};
	enc_desc.nextInChain = nullptr;
	enc_desc.label = {.data = "frame_encoder", .length = WGPU_STRLEN};
	this->encoder = wgpuDeviceCreateCommandEncoder(this->dev->GetDevice(), &enc_desc);

	return true;
}

/* -------------------------------------------------------------------------
 * SubmitSprites — render all sprite batches with depth testing
 * -------------------------------------------------------------------------*/

void GpuRenderer::SubmitSprites(const SpriteCommandBuffer &commands)
{
	if (this->encoder == nullptr) return;

	/* Ensure atlas bind groups cover all active pages. */
	uint16_t max_page = commands.MaxActivePage();
	if (max_page < SpriteCommandBuffer::MAX_ATLAS_PAGES) {
		this->EnsureAtlasBindGroups(max_page + 1);
	}

	/* Set up render pass with depth buffer clear. */
	WGPURenderPassColorAttachment color_att{};
	color_att.nextInChain = nullptr;
	color_att.view = this->surface_view;
	color_att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
	color_att.resolveTarget = nullptr;
	color_att.loadOp = WGPULoadOp_Clear;
	color_att.storeOp = WGPUStoreOp_Store;
	color_att.clearValue = {0.05, 0.05, 0.15, 1.0}; /* Dark blue background */

	WGPURenderPassDepthStencilAttachment depth_att{};
	depth_att.view = this->depth_view;
	depth_att.depthLoadOp = WGPULoadOp_Clear;
	depth_att.depthStoreOp = WGPUStoreOp_Store;
	depth_att.depthClearValue = 1.0f;
	depth_att.depthReadOnly = false;
	depth_att.stencilLoadOp = WGPULoadOp_Undefined;
	depth_att.stencilStoreOp = WGPUStoreOp_Undefined;
	depth_att.stencilClearValue = 0;
	depth_att.stencilReadOnly = true;

	WGPURenderPassDescriptor rp_desc{};
	rp_desc.nextInChain = nullptr;
	rp_desc.label = {.data = "sprite_pass", .length = WGPU_STRLEN};
	rp_desc.colorAttachmentCount = 1;
	rp_desc.colorAttachments = &color_att;
	rp_desc.depthStencilAttachment = &depth_att;
	rp_desc.occlusionQuerySet = nullptr;
	rp_desc.timestampWrites = nullptr;

	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(this->encoder, &rp_desc);
	wgpuRenderPassEncoderSetPipeline(pass, this->sprite_pipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, this->viewport_bind_group, 0, nullptr);

	/* Iterate per-page batches and draw instanced. */
	for (uint16_t page = 0; page <= max_page && page < SpriteCommandBuffer::MAX_ATLAS_PAGES; ++page) {
		size_t count = commands.PageCount(page);
		if (count == 0) continue;

		/* Ensure instance buffer is large enough. */
		this->EnsureInstanceBuffer(count);

		/* Upload instance data to GPU. */
		const GpuSpriteInstance *data = commands.PageData(page);
		wgpuQueueWriteBuffer(
			this->dev->GetQueue(),
			this->instance_buffer,
			0,
			data,
			count * sizeof(GpuSpriteInstance)
		);

		/* Bind atlas textures for this page. */
		if (page < this->atlas_bind_groups.size() && this->atlas_bind_groups[page] != nullptr) {
			wgpuRenderPassEncoderSetBindGroup(pass, 1, this->atlas_bind_groups[page], 0, nullptr);
		} else {
			continue; /* No bind group for this page, skip. */
		}

		/* Set instance buffer and draw. */
		wgpuRenderPassEncoderSetVertexBuffer(pass, 0, this->instance_buffer, 0, count * sizeof(GpuSpriteInstance));
		wgpuRenderPassEncoderDraw(pass, 4, static_cast<uint32_t>(count), 0, 0);
	}

	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);
}

/* -------------------------------------------------------------------------
 * CompositeUI — upload CPU buffer and draw fullscreen triangle
 * -------------------------------------------------------------------------*/

void GpuRenderer::CompositeUI(const void *pixels, int width, int height)
{
	if (this->encoder == nullptr) return;
	if (pixels == nullptr) return;

	/* Recreate UI texture if size changed. */
	if (width != this->ui_tex_width || height != this->ui_tex_height) {
		this->DestroyUIResources();
		this->CreateUIResources(width, height);
	}

	/* Upload the CPU BGRA buffer to the UI texture. */
	WGPUTexelCopyTextureInfo dst_info{};
	dst_info.texture = this->ui_texture;
	dst_info.mipLevel = 0;
	dst_info.origin = {0, 0, 0};
	dst_info.aspect = WGPUTextureAspect_All;

	WGPUTexelCopyBufferLayout layout{};
	layout.offset = 0;
	layout.bytesPerRow = static_cast<uint32_t>(width) * 4;
	layout.rowsPerImage = static_cast<uint32_t>(height);

	WGPUExtent3D extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};

	wgpuQueueWriteTexture(
		this->dev->GetQueue(),
		&dst_info,
		pixels,
		static_cast<size_t>(width) * height * 4,
		&layout,
		&extent
	);

	/* Render pass: Load (preserve sprites underneath), alpha-blend UI on top. */
	WGPURenderPassColorAttachment color_att{};
	color_att.nextInChain = nullptr;
	color_att.view = this->surface_view;
	color_att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
	color_att.resolveTarget = nullptr;
	color_att.loadOp = WGPULoadOp_Load;
	color_att.storeOp = WGPUStoreOp_Store;
	color_att.clearValue = {0.0, 0.0, 0.0, 1.0};

	WGPURenderPassDescriptor rp_desc{};
	rp_desc.nextInChain = nullptr;
	rp_desc.label = {.data = "ui_composite_pass", .length = WGPU_STRLEN};
	rp_desc.colorAttachmentCount = 1;
	rp_desc.colorAttachments = &color_att;
	rp_desc.depthStencilAttachment = nullptr;
	rp_desc.occlusionQuerySet = nullptr;
	rp_desc.timestampWrites = nullptr;

	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(this->encoder, &rp_desc);
	wgpuRenderPassEncoderSetPipeline(pass, this->ui_pipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, this->ui_bind_group, 0, nullptr);
	wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);
}

/* -------------------------------------------------------------------------
 * Present — submit and present
 * -------------------------------------------------------------------------*/

void GpuRenderer::Present()
{
	if (this->encoder == nullptr) return;

	/* Finish command encoder. */
	WGPUCommandBufferDescriptor cb_desc{};
	cb_desc.nextInChain = nullptr;
	cb_desc.label = {.data = "frame_cmdbuf", .length = WGPU_STRLEN};
	WGPUCommandBuffer cmd_buf = wgpuCommandEncoderFinish(this->encoder, &cb_desc);
	wgpuQueueSubmit(this->dev->GetQueue(), 1, &cmd_buf);
	wgpuCommandBufferRelease(cmd_buf);
	wgpuCommandEncoderRelease(this->encoder);
	this->encoder = nullptr;

	/* Release per-frame views. */
	if (this->surface_view != nullptr) {
		wgpuTextureViewRelease(this->surface_view);
		this->surface_view = nullptr;
	}

	/* Present. */
	WGPUStatus present_status = wgpuSurfacePresent(this->dev->GetSurface());
	if (present_status != WGPUStatus_Success) {
		Debug(driver, 0, "[gpu_renderer] Surface present failed, status={}", static_cast<int>(present_status));
	}

	/* Release the surface texture. */
	if (this->surface_texture.texture != nullptr) {
		wgpuTextureRelease(this->surface_texture.texture);
		this->surface_texture.texture = nullptr;
	}
}

/* -------------------------------------------------------------------------
 * Shutdown — release everything
 * -------------------------------------------------------------------------*/

void GpuRenderer::Shutdown()
{
	/* Per-frame state (in case of early shutdown). */
	if (this->encoder != nullptr) {
		wgpuCommandEncoderRelease(this->encoder);
		this->encoder = nullptr;
	}
	if (this->surface_view != nullptr) {
		wgpuTextureViewRelease(this->surface_view);
		this->surface_view = nullptr;
	}
	if (this->surface_texture.texture != nullptr) {
		wgpuTextureRelease(this->surface_texture.texture);
		this->surface_texture.texture = nullptr;
	}

	/* UI resources. */
	this->DestroyUIResources();
	if (this->ui_pipeline != nullptr) { wgpuRenderPipelineRelease(this->ui_pipeline); this->ui_pipeline = nullptr; }
	if (this->ui_bgl != nullptr) { wgpuBindGroupLayoutRelease(this->ui_bgl); this->ui_bgl = nullptr; }
	if (this->ui_shader != nullptr) { wgpuShaderModuleRelease(this->ui_shader); this->ui_shader = nullptr; }

	/* Depth buffer. */
	this->DestroyDepthBuffer();

	/* Instance buffer. */
	if (this->instance_buffer != nullptr) { wgpuBufferRelease(this->instance_buffer); this->instance_buffer = nullptr; }
	this->instance_buffer_capacity = 0;

	/* Atlas bind groups. */
	for (auto &bg : this->atlas_bind_groups) {
		if (bg != nullptr) { wgpuBindGroupRelease(bg); bg = nullptr; }
	}
	this->atlas_bind_groups.clear();

	/* Remap table + sampler. */
	this->remap_table.Shutdown();
	this->remap_table_built = false;
	if (this->remap_sampler != nullptr) { wgpuSamplerRelease(this->remap_sampler); this->remap_sampler = nullptr; }

	/* Atlas sampler. */
	if (this->atlas_sampler != nullptr) { wgpuSamplerRelease(this->atlas_sampler); this->atlas_sampler = nullptr; }

	/* Viewport. */
	if (this->viewport_bind_group != nullptr) { wgpuBindGroupRelease(this->viewport_bind_group); this->viewport_bind_group = nullptr; }
	if (this->viewport_uniform_buf != nullptr) { wgpuBufferRelease(this->viewport_uniform_buf); this->viewport_uniform_buf = nullptr; }
	if (this->viewport_bgl != nullptr) { wgpuBindGroupLayoutRelease(this->viewport_bgl); this->viewport_bgl = nullptr; }

	/* Atlas BGL. */
	if (this->atlas_bgl != nullptr) { wgpuBindGroupLayoutRelease(this->atlas_bgl); this->atlas_bgl = nullptr; }

	/* Sprite pipeline. */
	if (this->sprite_pipeline != nullptr) { wgpuRenderPipelineRelease(this->sprite_pipeline); this->sprite_pipeline = nullptr; }
	if (this->sprite_shader != nullptr) { wgpuShaderModuleRelease(this->sprite_shader); this->sprite_shader = nullptr; }

	this->dev = nullptr;

	Debug(driver, 0, "[gpu_renderer] Shutdown complete");
}

/* -------------------------------------------------------------------------
 * Helper: CreateDepthBuffer
 * -------------------------------------------------------------------------*/

void GpuRenderer::CreateDepthBuffer(int w, int h)
{
	WGPUTextureDescriptor desc{};
	desc.nextInChain = nullptr;
	desc.label = {.data = "depth_texture", .length = WGPU_STRLEN};
	desc.usage = WGPUTextureUsage_RenderAttachment;
	desc.dimension = WGPUTextureDimension_2D;
	desc.size = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
	desc.format = WGPUTextureFormat_Depth24Plus;
	desc.mipLevelCount = 1;
	desc.sampleCount = 1;
	desc.viewFormatCount = 0;
	desc.viewFormats = nullptr;
	this->depth_texture = wgpuDeviceCreateTexture(this->dev->GetDevice(), &desc);

	WGPUTextureViewDescriptor view_desc{};
	view_desc.nextInChain = nullptr;
	view_desc.label = {.data = "depth_view", .length = WGPU_STRLEN};
	view_desc.format = WGPUTextureFormat_Depth24Plus;
	view_desc.dimension = WGPUTextureViewDimension_2D;
	view_desc.baseMipLevel = 0;
	view_desc.mipLevelCount = 1;
	view_desc.baseArrayLayer = 0;
	view_desc.arrayLayerCount = 1;
	view_desc.aspect = WGPUTextureAspect_DepthOnly;
	view_desc.usage = WGPUTextureUsage_RenderAttachment;
	this->depth_view = wgpuTextureCreateView(this->depth_texture, &view_desc);
}

void GpuRenderer::DestroyDepthBuffer()
{
	if (this->depth_view != nullptr) { wgpuTextureViewRelease(this->depth_view); this->depth_view = nullptr; }
	if (this->depth_texture != nullptr) { wgpuTextureRelease(this->depth_texture); this->depth_texture = nullptr; }
}

/* -------------------------------------------------------------------------
 * Helper: Viewport uniforms
 * -------------------------------------------------------------------------*/

void GpuRenderer::CreateViewportUniform()
{
	WGPUBufferDescriptor desc{};
	desc.nextInChain = nullptr;
	desc.label = {.data = "viewport_uniform", .length = WGPU_STRLEN};
	desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
	desc.size = 16; /* 4 floats: scale.x, scale.y, offset.x, offset.y */
	desc.mappedAtCreation = false;
	this->viewport_uniform_buf = wgpuDeviceCreateBuffer(this->dev->GetDevice(), &desc);

	/* Create bind group. */
	WGPUBindGroupEntry entry{};
	entry.nextInChain = nullptr;
	entry.binding = 0;
	entry.buffer = this->viewport_uniform_buf;
	entry.offset = 0;
	entry.size = 16;
	entry.sampler = nullptr;
	entry.textureView = nullptr;

	WGPUBindGroupDescriptor bg_desc{};
	bg_desc.nextInChain = nullptr;
	bg_desc.label = {.data = "viewport_bind_group", .length = WGPU_STRLEN};
	bg_desc.layout = this->viewport_bgl;
	bg_desc.entryCount = 1;
	bg_desc.entries = &entry;
	this->viewport_bind_group = wgpuDeviceCreateBindGroup(this->dev->GetDevice(), &bg_desc);
}

void GpuRenderer::UpdateViewportUniform(int w, int h)
{
	/* scale = (2.0/w, -2.0/h), offset = (-1.0, 1.0)
	 * Converts screen pixel (x,y) to NDC: ndc = pixel * scale + offset */
	float data[4] = {
		2.0f / static_cast<float>(w),
		-2.0f / static_cast<float>(h),
		-1.0f,
		1.0f,
	};

	wgpuQueueWriteBuffer(
		this->dev->GetQueue(),
		this->viewport_uniform_buf,
		0,
		data,
		sizeof(data)
	);
}

/* -------------------------------------------------------------------------
 * Helper: EnsureInstanceBuffer — grow to power-of-2
 * -------------------------------------------------------------------------*/

void GpuRenderer::EnsureInstanceBuffer(size_t required_count)
{
	if (required_count <= this->instance_buffer_capacity) return;

	/* Grow to next power of 2, minimum 256. */
	size_t new_cap = 256;
	while (new_cap < required_count) new_cap *= 2;

	if (this->instance_buffer != nullptr) {
		wgpuBufferRelease(this->instance_buffer);
	}

	WGPUBufferDescriptor desc{};
	desc.nextInChain = nullptr;
	desc.label = {.data = "instance_buffer", .length = WGPU_STRLEN};
	desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
	desc.size = new_cap * sizeof(GpuSpriteInstance);
	desc.mappedAtCreation = false;
	this->instance_buffer = wgpuDeviceCreateBuffer(this->dev->GetDevice(), &desc);
	this->instance_buffer_capacity = new_cap;

	Debug(driver, 1, "[gpu_renderer] Instance buffer grown to {} entries ({} bytes)",
		new_cap, new_cap * sizeof(GpuSpriteInstance));
}

/* -------------------------------------------------------------------------
 * Helper: EnsureAtlasBindGroups — lazily create per-page bind groups
 * -------------------------------------------------------------------------*/

void GpuRenderer::EnsureRemapTable()
{
	if (this->remap_table_built) return;
	if (this->remap_table.Build()) {
		this->remap_table_built = true;
	}
}

void GpuRenderer::EnsureAtlasBindGroups(uint16_t page_count)
{
	if (_sprite_atlas == nullptr) return;

	/* Ensure the remap table is built before creating bind groups. */
	this->EnsureRemapTable();

	/* Grow vector if needed. */
	while (this->atlas_bind_groups.size() < page_count) {
		this->atlas_bind_groups.push_back(nullptr);
	}

	WGPUDevice d = this->dev->GetDevice();

	/* Get remap table texture (may be nullptr if build failed). */
	WGPUTexture remap_tex = this->remap_table.GetTexture();
	if (remap_tex == nullptr) return;

	for (uint16_t page = 0; page < page_count; ++page) {
		/* Skip pages that already have a bind group. */
		if (this->atlas_bind_groups[page] != nullptr) continue;

		WGPUTexture rgba_tex = _sprite_atlas->GetRGBATexture(page);
		WGPUTexture m_tex = _sprite_atlas->GetMTexture(page);
		if (rgba_tex == nullptr || m_tex == nullptr) continue;

		/* Create texture views. */
		WGPUTextureViewDescriptor view_desc{};
		view_desc.nextInChain = nullptr;
		view_desc.format = WGPUTextureFormat_RGBA8Unorm;
		view_desc.dimension = WGPUTextureViewDimension_2D;
		view_desc.baseMipLevel = 0;
		view_desc.mipLevelCount = 1;
		view_desc.baseArrayLayer = 0;
		view_desc.arrayLayerCount = 1;
		view_desc.aspect = WGPUTextureAspect_All;
		view_desc.usage = WGPUTextureUsage_TextureBinding;

		view_desc.label = {.data = "atlas_rgba_view", .length = WGPU_STRLEN};
		WGPUTextureView rgba_view = wgpuTextureCreateView(rgba_tex, &view_desc);

		/* M texture uses R8Unorm format. */
		view_desc.label = {.data = "atlas_m_view", .length = WGPU_STRLEN};
		view_desc.format = WGPUTextureFormat_R8Unorm;
		WGPUTextureView m_view = wgpuTextureCreateView(m_tex, &view_desc);

		/* Remap table texture view. */
		view_desc.label = {.data = "remap_table_view", .length = WGPU_STRLEN};
		view_desc.format = WGPUTextureFormat_RGBA8Unorm;
		WGPUTextureView remap_view = wgpuTextureCreateView(remap_tex, &view_desc);

		/* Create bind group: rgba, m, atlas_sampler, remap_table, remap_sampler. */
		WGPUBindGroupEntry entries[5]{};

		entries[0].nextInChain = nullptr;
		entries[0].binding = 0;
		entries[0].textureView = rgba_view;
		entries[0].buffer = nullptr;
		entries[0].offset = 0;
		entries[0].size = 0;
		entries[0].sampler = nullptr;

		entries[1].nextInChain = nullptr;
		entries[1].binding = 1;
		entries[1].textureView = m_view;
		entries[1].buffer = nullptr;
		entries[1].offset = 0;
		entries[1].size = 0;
		entries[1].sampler = nullptr;

		entries[2].nextInChain = nullptr;
		entries[2].binding = 2;
		entries[2].sampler = this->atlas_sampler;
		entries[2].textureView = nullptr;
		entries[2].buffer = nullptr;
		entries[2].offset = 0;
		entries[2].size = 0;

		entries[3].nextInChain = nullptr;
		entries[3].binding = 3;
		entries[3].textureView = remap_view;
		entries[3].buffer = nullptr;
		entries[3].offset = 0;
		entries[3].size = 0;
		entries[3].sampler = nullptr;

		entries[4].nextInChain = nullptr;
		entries[4].binding = 4;
		entries[4].sampler = this->remap_sampler;
		entries[4].textureView = nullptr;
		entries[4].buffer = nullptr;
		entries[4].offset = 0;
		entries[4].size = 0;

		WGPUBindGroupDescriptor bg_desc{};
		bg_desc.nextInChain = nullptr;
		bg_desc.label = {.data = "atlas_bind_group", .length = WGPU_STRLEN};
		bg_desc.layout = this->atlas_bgl;
		bg_desc.entryCount = 5;
		bg_desc.entries = entries;
		this->atlas_bind_groups[page] = wgpuDeviceCreateBindGroup(d, &bg_desc);

		/* Views are now owned by the bind group. */
		wgpuTextureViewRelease(rgba_view);
		wgpuTextureViewRelease(m_view);
		wgpuTextureViewRelease(remap_view);

		Debug(driver, 1, "[gpu_renderer] Created atlas bind group for page {}", page);
	}
}

/* -------------------------------------------------------------------------
 * Helper: UI resources
 * -------------------------------------------------------------------------*/

void GpuRenderer::CreateUIResources(int w, int h)
{
	WGPUDevice d = this->dev->GetDevice();
	this->ui_tex_width = w;
	this->ui_tex_height = h;

	/* 1. UI texture — RGBA8Unorm, CopyDst + TextureBinding. */
	WGPUTextureDescriptor tex_desc{};
	tex_desc.nextInChain = nullptr;
	tex_desc.label = {.data = "ui_texture", .length = WGPU_STRLEN};
	tex_desc.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
	tex_desc.dimension = WGPUTextureDimension_2D;
	tex_desc.size = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
	tex_desc.format = WGPUTextureFormat_BGRA8Unorm;
	tex_desc.mipLevelCount = 1;
	tex_desc.sampleCount = 1;
	tex_desc.viewFormatCount = 0;
	tex_desc.viewFormats = nullptr;
	this->ui_texture = wgpuDeviceCreateTexture(d, &tex_desc);

	/* 2. Texture view. */
	WGPUTextureViewDescriptor tv_desc{};
	tv_desc.nextInChain = nullptr;
	tv_desc.label = {.data = "ui_texture_view", .length = WGPU_STRLEN};
	tv_desc.format = WGPUTextureFormat_BGRA8Unorm;
	tv_desc.dimension = WGPUTextureViewDimension_2D;
	tv_desc.baseMipLevel = 0;
	tv_desc.mipLevelCount = 1;
	tv_desc.baseArrayLayer = 0;
	tv_desc.arrayLayerCount = 1;
	tv_desc.aspect = WGPUTextureAspect_All;
	tv_desc.usage = WGPUTextureUsage_TextureBinding;
	this->ui_texture_view = wgpuTextureCreateView(this->ui_texture, &tv_desc);

	/* 3. UI bind group layout (only created once). */
	if (this->ui_bgl == nullptr) {
		WGPUBindGroupLayoutEntry entries[2]{};

		entries[0].nextInChain = nullptr;
		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Fragment;
		entries[0].texture.sampleType = WGPUTextureSampleType_Float;
		entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[0].texture.multisampled = false;
		entries[0].buffer = {};
		entries[0].sampler = {};
		entries[0].storageTexture = {};

		entries[1].nextInChain = nullptr;
		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Fragment;
		entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
		entries[1].buffer = {};
		entries[1].texture = {};
		entries[1].storageTexture = {};

		WGPUBindGroupLayoutDescriptor bgl_desc{};
		bgl_desc.nextInChain = nullptr;
		bgl_desc.label = {.data = "ui_bgl", .length = WGPU_STRLEN};
		bgl_desc.entryCount = 2;
		bgl_desc.entries = entries;
		this->ui_bgl = wgpuDeviceCreateBindGroupLayout(d, &bgl_desc);
	}

	/* 4. UI sampler (only created once). */
	if (this->ui_sampler == nullptr) {
		WGPUSamplerDescriptor samp_desc{};
		samp_desc.nextInChain = nullptr;
		samp_desc.label = {.data = "ui_sampler", .length = WGPU_STRLEN};
		samp_desc.addressModeU = WGPUAddressMode_ClampToEdge;
		samp_desc.addressModeV = WGPUAddressMode_ClampToEdge;
		samp_desc.addressModeW = WGPUAddressMode_ClampToEdge;
		samp_desc.magFilter = WGPUFilterMode_Nearest;
		samp_desc.minFilter = WGPUFilterMode_Nearest;
		samp_desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
		samp_desc.lodMinClamp = 0.0f;
		samp_desc.lodMaxClamp = 1.0f;
		samp_desc.compare = WGPUCompareFunction_Undefined;
		samp_desc.maxAnisotropy = 1;
		this->ui_sampler = wgpuDeviceCreateSampler(d, &samp_desc);
	}

	/* 5. UI pipeline (only created once). */
	if (this->ui_pipeline == nullptr) {
		WGPUPipelineLayoutDescriptor pl_desc{};
		pl_desc.nextInChain = nullptr;
		pl_desc.label = {.data = "ui_pipeline_layout", .length = WGPU_STRLEN};
		pl_desc.bindGroupLayoutCount = 1;
		pl_desc.bindGroupLayouts = &this->ui_bgl;
		WGPUPipelineLayout ui_pl = wgpuDeviceCreatePipelineLayout(d, &pl_desc);

		/* Alpha blending for UI overlay. */
		WGPUBlendState blend{};
		blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
		blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend.color.operation = WGPUBlendOperation_Add;
		blend.alpha.srcFactor = WGPUBlendFactor_One;
		blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend.alpha.operation = WGPUBlendOperation_Add;

		WGPUColorTargetState color_target{};
		color_target.nextInChain = nullptr;
		color_target.format = this->dev->GetSurfaceFormat();
		color_target.blend = &blend;
		color_target.writeMask = WGPUColorWriteMask_All;

		WGPUFragmentState frag_state{};
		frag_state.nextInChain = nullptr;
		frag_state.module = this->ui_shader;
		frag_state.entryPoint = {.data = "fs_ui", .length = WGPU_STRLEN};
		frag_state.constantCount = 0;
		frag_state.constants = nullptr;
		frag_state.targetCount = 1;
		frag_state.targets = &color_target;

		WGPURenderPipelineDescriptor rp_desc{};
		rp_desc.nextInChain = nullptr;
		rp_desc.label = {.data = "ui_pipeline", .length = WGPU_STRLEN};
		rp_desc.layout = ui_pl;

		rp_desc.vertex.nextInChain = nullptr;
		rp_desc.vertex.module = this->ui_shader;
		rp_desc.vertex.entryPoint = {.data = "vs_ui", .length = WGPU_STRLEN};
		rp_desc.vertex.constantCount = 0;
		rp_desc.vertex.constants = nullptr;
		rp_desc.vertex.bufferCount = 0;
		rp_desc.vertex.buffers = nullptr;

		rp_desc.primitive.nextInChain = nullptr;
		rp_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		rp_desc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
		rp_desc.primitive.frontFace = WGPUFrontFace_CCW;
		rp_desc.primitive.cullMode = WGPUCullMode_None;
		rp_desc.primitive.unclippedDepth = false;

		rp_desc.depthStencil = nullptr;

		rp_desc.multisample.nextInChain = nullptr;
		rp_desc.multisample.count = 1;
		rp_desc.multisample.mask = 0xFFFFFFFF;
		rp_desc.multisample.alphaToCoverageEnabled = false;

		rp_desc.fragment = &frag_state;

		this->ui_pipeline = wgpuDeviceCreateRenderPipeline(d, &rp_desc);
		Debug(driver, 0, "[gpu_renderer] ui_pipeline = {}", (void *)this->ui_pipeline);

		wgpuPipelineLayoutRelease(ui_pl);
	}

	/* 6. UI bind group (recreated on each resize since it references the texture view). */
	WGPUBindGroupEntry bg_entries[2]{};
	bg_entries[0].nextInChain = nullptr;
	bg_entries[0].binding = 0;
	bg_entries[0].textureView = this->ui_texture_view;
	bg_entries[0].buffer = nullptr;
	bg_entries[0].offset = 0;
	bg_entries[0].size = 0;
	bg_entries[0].sampler = nullptr;

	bg_entries[1].nextInChain = nullptr;
	bg_entries[1].binding = 1;
	bg_entries[1].sampler = this->ui_sampler;
	bg_entries[1].textureView = nullptr;
	bg_entries[1].buffer = nullptr;
	bg_entries[1].offset = 0;
	bg_entries[1].size = 0;

	WGPUBindGroupDescriptor bg_desc{};
	bg_desc.nextInChain = nullptr;
	bg_desc.label = {.data = "ui_bind_group", .length = WGPU_STRLEN};
	bg_desc.layout = this->ui_bgl;
	bg_desc.entryCount = 2;
	bg_desc.entries = bg_entries;
	this->ui_bind_group = wgpuDeviceCreateBindGroup(d, &bg_desc);
}

void GpuRenderer::DestroyUIResources()
{
	if (this->ui_bind_group != nullptr) { wgpuBindGroupRelease(this->ui_bind_group); this->ui_bind_group = nullptr; }
	if (this->ui_texture_view != nullptr) { wgpuTextureViewRelease(this->ui_texture_view); this->ui_texture_view = nullptr; }
	if (this->ui_texture != nullptr) { wgpuTextureRelease(this->ui_texture); this->ui_texture = nullptr; }
	this->ui_tex_width = 0;
	this->ui_tex_height = 0;
}

#endif /* WITH_WGPU */

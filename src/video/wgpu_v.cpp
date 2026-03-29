/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file wgpu_v.cpp Implementation of the wgpu video driver. */

#ifdef WITH_WGPU

#include "../stdafx.h"
#include "../openttd.h"
#include "../gfx_func.h"
#include "../blitter/factory.hpp"
#include "../thread.h"
#include "../progress.h"
#include "../core/geometry_func.hpp"
#include "../core/utf8.hpp"
#include "../fileio_func.h"
#include "../framerate_type.h"
#include "../window_func.h"
#include "wgpu_v.h"
#include "../gpu/sprite_atlas.h"
#include <SDL.h>
#ifdef __APPLE__
#	include <SDL_metal.h>
#endif

#include "../safeguards.h"

/* -------------------------------------------------------------------------
 * Key mapping — duplicated from sdl2_v.cpp so this driver is self-contained.
 * -------------------------------------------------------------------------*/

struct SDLVkMapping_Wgpu {
	const SDL_Keycode vk_from;
	const uint8_t vk_count;
	const uint8_t map_to;
	const bool unprintable;

	constexpr SDLVkMapping_Wgpu(SDL_Keycode vk_first, SDL_Keycode vk_last, uint8_t map_first, [[maybe_unused]] uint8_t map_last, bool unprintable)
		: vk_from(vk_first), vk_count(vk_last - vk_first + 1), map_to(map_first), unprintable(unprintable)
	{
		assert((vk_last - vk_first) == (map_last - map_first));
	}
};

#define WAS(x, z)    {x, x, z, z, false}
#define WAM(x, y, z, w) {x, y, z, w, false}
#define WAS_UP(x, z) {x, x, z, z, true}
#define WAM_UP(x, y, z, w) {x, y, z, w, true}

static constexpr SDLVkMapping_Wgpu _wgpu_vk_mapping[] = {
	WAS_UP(SDLK_PAGEUP,   WKC_PAGEUP),
	WAS_UP(SDLK_PAGEDOWN, WKC_PAGEDOWN),
	WAS_UP(SDLK_UP,       WKC_UP),
	WAS_UP(SDLK_DOWN,     WKC_DOWN),
	WAS_UP(SDLK_LEFT,     WKC_LEFT),
	WAS_UP(SDLK_RIGHT,    WKC_RIGHT),
	WAS_UP(SDLK_HOME,     WKC_HOME),
	WAS_UP(SDLK_END,      WKC_END),
	WAS_UP(SDLK_INSERT,   WKC_INSERT),
	WAS_UP(SDLK_DELETE,   WKC_DELETE),
	WAM(SDLK_a, SDLK_z, 'A', 'Z'),
	WAM(SDLK_0, SDLK_9, '0', '9'),
	WAS_UP(SDLK_ESCAPE,    WKC_ESC),
	WAS_UP(SDLK_PAUSE,     WKC_PAUSE),
	WAS_UP(SDLK_BACKSPACE, WKC_BACKSPACE),
	WAS(SDLK_SPACE,     WKC_SPACE),
	WAS(SDLK_RETURN,    WKC_RETURN),
	WAS(SDLK_TAB,       WKC_TAB),
	WAM_UP(SDLK_F1, SDLK_F12, WKC_F1, WKC_F12),
	WAS(SDLK_KP_1,        '1'),
	WAS(SDLK_KP_2,        '2'),
	WAS(SDLK_KP_3,        '3'),
	WAS(SDLK_KP_4,        '4'),
	WAS(SDLK_KP_5,        '5'),
	WAS(SDLK_KP_6,        '6'),
	WAS(SDLK_KP_7,        '7'),
	WAS(SDLK_KP_8,        '8'),
	WAS(SDLK_KP_9,        '9'),
	WAS(SDLK_KP_0,        '0'),
	WAS(SDLK_KP_DIVIDE,   WKC_NUM_DIV),
	WAS(SDLK_KP_MULTIPLY, WKC_NUM_MUL),
	WAS(SDLK_KP_MINUS,    WKC_NUM_MINUS),
	WAS(SDLK_KP_PLUS,     WKC_NUM_PLUS),
	WAS(SDLK_KP_ENTER,    WKC_NUM_ENTER),
	WAS(SDLK_KP_PERIOD,   WKC_NUM_DECIMAL),
	WAS(SDLK_SLASH,        WKC_SLASH),
	WAS(SDLK_SEMICOLON,    WKC_SEMICOLON),
	WAS(SDLK_EQUALS,       WKC_EQUALS),
	WAS(SDLK_LEFTBRACKET,  WKC_L_BRACKET),
	WAS(SDLK_BACKSLASH,    WKC_BACKSLASH),
	WAS(SDLK_RIGHTBRACKET, WKC_R_BRACKET),
	WAS(SDLK_QUOTE,   WKC_SINGLEQUOTE),
	WAS(SDLK_COMMA,   WKC_COMMA),
	WAS(SDLK_MINUS,   WKC_MINUS),
	WAS(SDLK_PERIOD,  WKC_PERIOD),
};

#undef WAS
#undef WAM
#undef WAS_UP
#undef WAM_UP

static uint ConvertSdlKeyIntoMy_Wgpu(SDL_Keysym *sym, char32_t *character)
{
	uint key = 0;
	bool unprintable = false;

	for (const auto &map : _wgpu_vk_mapping) {
		if (IsInsideBS(sym->sym, map.vk_from, map.vk_count)) {
			key = sym->sym - map.vk_from + map.map_to;
			unprintable = map.unprintable;
			break;
		}
	}

	if (sym->scancode == SDL_SCANCODE_GRAVE) key = WKC_BACKQUOTE;

	if (sym->mod & KMOD_GUI)   key |= WKC_META;
	if (sym->mod & KMOD_SHIFT) key |= WKC_SHIFT;
	if (sym->mod & KMOD_CTRL)  key |= WKC_CTRL;
	if (sym->mod & KMOD_ALT)   key |= WKC_ALT;

	if (sym->mod & KMOD_GUI ||
		sym->mod & KMOD_CTRL ||
		sym->mod & KMOD_ALT ||
		unprintable) {
		*character = WKC_NONE;
	} else {
		*character = sym->sym;
	}

	return key;
}

static uint ConvertSdlKeycodeIntoMy_Wgpu(SDL_Keycode kc)
{
	uint key = 0;

	for (const auto &map : _wgpu_vk_mapping) {
		if (IsInsideBS(kc, map.vk_from, map.vk_count)) {
			key = kc - map.vk_from + map.map_to;
			break;
		}
	}

	SDL_Scancode sc = SDL_GetScancodeFromKey(kc);
	if (sc == SDL_SCANCODE_GRAVE) key = WKC_BACKQUOTE;

	return key;
}

/* -------------------------------------------------------------------------
 * Blit resources — intermediate texture + fullscreen triangle pipeline.
 * -------------------------------------------------------------------------*/

/** WGSL shader for fullscreen blit: vertex produces a large triangle,
 *  fragment samples the CPU-uploaded screen texture. */
static const char *const kBlitShaderWGSL = R"(
struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs(@builtin(vertex_index) vi: u32) -> VertexOutput {
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

@group(0) @binding(0) var screen_tex: texture_2d<f32>;
@group(0) @binding(1) var screen_sampler: sampler;

@fragment fn fs(in: VertexOutput) -> @location(0) vec4f {
    return textureSample(screen_tex, screen_sampler, in.uv);
}
)";

void VideoDriver_Wgpu::CreateBlitResources(int w, int h)
{
	WGPUDevice device = this->gpu_device.GetDevice();

	/* 1. Screen texture — CopyDst so we can upload CPU pixels, TextureBinding for sampling. */
	WGPUTextureDescriptor tex_desc{};
	tex_desc.nextInChain = nullptr;
	tex_desc.label = {.data = "screen_texture", .length = WGPU_STRLEN};
	tex_desc.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
	tex_desc.dimension = WGPUTextureDimension_2D;
	tex_desc.size = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
	tex_desc.format = WGPUTextureFormat_BGRA8Unorm;
	tex_desc.mipLevelCount = 1;
	tex_desc.sampleCount = 1;
	tex_desc.viewFormatCount = 0;
	tex_desc.viewFormats = nullptr;
	this->screen_texture = wgpuDeviceCreateTexture(device, &tex_desc);
	Debug(driver, 0, "[wgpu] screen_texture={}", (void *)this->screen_texture);

	/* 2. Nearest-neighbour sampler — pixel-perfect blit. */
	WGPUSamplerDescriptor samp_desc{};
	samp_desc.nextInChain = nullptr;
	samp_desc.label = {.data = "blit_sampler", .length = WGPU_STRLEN};
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
	this->blit_sampler = wgpuDeviceCreateSampler(device, &samp_desc);
	Debug(driver, 0, "[wgpu] sampler={}", (void *)this->blit_sampler);

	/* 3. Compile WGSL shader module. */
	WGPUShaderSourceWGSL wgsl_src{};
	wgsl_src.chain.next = nullptr;
	wgsl_src.chain.sType = WGPUSType_ShaderSourceWGSL;
	wgsl_src.code = {.data = kBlitShaderWGSL, .length = WGPU_STRLEN};

	WGPUShaderModuleDescriptor sm_desc{};
	sm_desc.nextInChain = &wgsl_src.chain;
	sm_desc.label = {.data = "blit_shader", .length = WGPU_STRLEN};

	WGPUShaderModule shader = wgpuDeviceCreateShaderModule(device, &sm_desc);
	Debug(driver, 0, "[wgpu] shader={}", (void *)shader);

	/* 4. Bind group layout: texture_2d<f32> + sampler (fragment-only). */
	WGPUBindGroupLayoutEntry bgl_entries[2]{};

	/* binding 0 — texture */
	bgl_entries[0].nextInChain = nullptr;
	bgl_entries[0].binding = 0;
	bgl_entries[0].visibility = WGPUShaderStage_Fragment;
	bgl_entries[0].texture.sampleType = WGPUTextureSampleType_Float;
	bgl_entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
	bgl_entries[0].texture.multisampled = false;
	/* Zero-init the unused union members. */
	bgl_entries[0].buffer = {};
	bgl_entries[0].sampler = {};
	bgl_entries[0].storageTexture = {};

	/* binding 1 — sampler */
	bgl_entries[1].nextInChain = nullptr;
	bgl_entries[1].binding = 1;
	bgl_entries[1].visibility = WGPUShaderStage_Fragment;
	bgl_entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
	bgl_entries[1].buffer = {};
	bgl_entries[1].texture = {};
	bgl_entries[1].storageTexture = {};

	WGPUBindGroupLayoutDescriptor bgl_desc{};
	bgl_desc.nextInChain = nullptr;
	bgl_desc.label = {.data = "blit_bgl", .length = WGPU_STRLEN};
	bgl_desc.entryCount = 2;
	bgl_desc.entries = bgl_entries;
	this->blit_bgl = wgpuDeviceCreateBindGroupLayout(device, &bgl_desc);
	Debug(driver, 0, "[wgpu] bgl={}", (void *)this->blit_bgl);

	/* 5. Pipeline layout from blit_bgl. */
	WGPUPipelineLayoutDescriptor pl_desc{};
	pl_desc.nextInChain = nullptr;
	pl_desc.label = {.data = "blit_pipeline_layout", .length = WGPU_STRLEN};
	pl_desc.bindGroupLayoutCount = 1;
	pl_desc.bindGroupLayouts = &this->blit_bgl;
	WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pl_desc);

	/* 6. Render pipeline — fullscreen triangle, no vertex buffers. */
	WGPUColorTargetState color_target{};
	color_target.nextInChain = nullptr;
	color_target.format = this->gpu_device.GetSurfaceFormat();
	color_target.blend = nullptr;
	color_target.writeMask = WGPUColorWriteMask_All;

	WGPUFragmentState frag_state{};
	frag_state.nextInChain = nullptr;
	frag_state.module = shader;
	frag_state.entryPoint = {.data = "fs", .length = WGPU_STRLEN};
	frag_state.constantCount = 0;
	frag_state.constants = nullptr;
	frag_state.targetCount = 1;
	frag_state.targets = &color_target;

	WGPURenderPipelineDescriptor rp_desc{};
	rp_desc.nextInChain = nullptr;
	rp_desc.label = {.data = "blit_pipeline", .length = WGPU_STRLEN};
	rp_desc.layout = pipeline_layout;

	rp_desc.vertex.nextInChain = nullptr;
	rp_desc.vertex.module = shader;
	rp_desc.vertex.entryPoint = {.data = "vs", .length = WGPU_STRLEN};
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

	this->blit_pipeline = wgpuDeviceCreateRenderPipeline(device, &rp_desc);
	Debug(driver, 0, "[wgpu] pipeline={}", (void *)this->blit_pipeline);

	/* Pipeline layout is now owned by the pipeline — release our ref. */
	wgpuPipelineLayoutRelease(pipeline_layout);
	/* Shader module is now owned by the pipeline — release our ref. */
	wgpuShaderModuleRelease(shader);

	/* 7. Bind group: screen texture view + sampler. */
	WGPUTextureViewDescriptor tv_desc{};
	tv_desc.nextInChain = nullptr;
	tv_desc.label = {.data = "screen_tex_view", .length = WGPU_STRLEN};
	tv_desc.format = WGPUTextureFormat_BGRA8Unorm;
	tv_desc.dimension = WGPUTextureViewDimension_2D;
	tv_desc.baseMipLevel = 0;
	tv_desc.mipLevelCount = 1;
	tv_desc.baseArrayLayer = 0;
	tv_desc.arrayLayerCount = 1;
	tv_desc.aspect = WGPUTextureAspect_All;
	tv_desc.usage = WGPUTextureUsage_TextureBinding;
	WGPUTextureView tex_view = wgpuTextureCreateView(this->screen_texture, &tv_desc);

	WGPUBindGroupEntry bg_entries[2]{};
	bg_entries[0].nextInChain = nullptr;
	bg_entries[0].binding = 0;
	bg_entries[0].textureView = tex_view;
	bg_entries[0].buffer = nullptr;
	bg_entries[0].offset = 0;
	bg_entries[0].size = 0;
	bg_entries[0].sampler = nullptr;

	bg_entries[1].nextInChain = nullptr;
	bg_entries[1].binding = 1;
	bg_entries[1].sampler = this->blit_sampler;
	bg_entries[1].textureView = nullptr;
	bg_entries[1].buffer = nullptr;
	bg_entries[1].offset = 0;
	bg_entries[1].size = 0;

	WGPUBindGroupDescriptor bg_desc{};
	bg_desc.nextInChain = nullptr;
	bg_desc.label = {.data = "blit_bind_group", .length = WGPU_STRLEN};
	bg_desc.layout = this->blit_bgl;
	bg_desc.entryCount = 2;
	bg_desc.entries = bg_entries;
	this->blit_bind_group = wgpuDeviceCreateBindGroup(device, &bg_desc);
	Debug(driver, 0, "[wgpu] bind_group={}", (void *)this->blit_bind_group);

	/* Texture view is now owned by the bind group — release our ref. */
	wgpuTextureViewRelease(tex_view);
}

void VideoDriver_Wgpu::GetDrawableSize(int &w, int &h) const
{
#ifdef __APPLE__
	SDL_Metal_GetDrawableSize(this->sdl_window, &w, &h);
#else
	SDL_GL_GetDrawableSize(this->sdl_window, &w, &h);
#endif
	if (w < 64) w = 64;
	if (h < 64) h = 64;
}

void VideoDriver_Wgpu::DestroyBlitResources()
{
	if (this->blit_bind_group != nullptr) { wgpuBindGroupRelease(this->blit_bind_group); this->blit_bind_group = nullptr; }
	if (this->blit_pipeline != nullptr)   { wgpuRenderPipelineRelease(this->blit_pipeline); this->blit_pipeline = nullptr; }
	if (this->blit_bgl != nullptr)        { wgpuBindGroupLayoutRelease(this->blit_bgl); this->blit_bgl = nullptr; }
	if (this->blit_sampler != nullptr)     { wgpuSamplerRelease(this->blit_sampler); this->blit_sampler = nullptr; }
	if (this->screen_texture != nullptr)   { wgpuTextureRelease(this->screen_texture); this->screen_texture = nullptr; }
}

/* -------------------------------------------------------------------------
 * VideoDriver_Wgpu — Start / Stop
 * -------------------------------------------------------------------------*/

std::optional<std::string_view> VideoDriver_Wgpu::Start(const StringList &param)
{
	this->UpdateAutoResolution();

	/* Initialise SDL video subsystem. */
	if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
#ifdef SDL_HINT_APP_NAME
		SDL_SetHint(SDL_HINT_APP_NAME, "OpenTTD");
#endif
		if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return SDL_GetError();
	}

	/* Select a 32-bpp blitter — UI code still writes to the CPU buffer. */
	if (BlitterFactory::SelectBlitter("32bpp-simple") == nullptr) {
		/* Fall back gracefully if the blitter isn't found. */
		Debug(driver, 1, "wgpu: 32bpp-simple blitter not found, trying fallback");
		if (BlitterFactory::GetCurrentBlitter() == nullptr ||
			BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 0) {
			return "No suitable blitter available for wgpu driver";
		}
	}

	uint w = _cur_resolution.width;
	uint h = _cur_resolution.height;

	/* Create the SDL2 window with Metal support on macOS. */
	uint32_t flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
#ifdef __APPLE__
	flags |= SDL_WINDOW_METAL;
#endif

	std::string caption = VideoDriver::GetCaption();
	this->sdl_window = SDL_CreateWindow(
		caption.c_str(),
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		w, h,
		flags);
	if (this->sdl_window == nullptr) return SDL_GetError();

	/* Get native layer for wgpu surface creation. */
	void *native_layer = nullptr;
#ifdef __APPLE__
	this->metal_view = SDL_Metal_CreateView(this->sdl_window);
	if (this->metal_view == nullptr) {
		SDL_DestroyWindow(this->sdl_window);
		this->sdl_window = nullptr;
		return "wgpu: SDL_Metal_CreateView failed";
	}
	native_layer = SDL_Metal_GetLayer(this->metal_view);
#endif

	/* Initialise the wgpu device. */
	if (!this->gpu_device.Init(native_layer, static_cast<int>(w), static_cast<int>(h))) {
#ifdef __APPLE__
		SDL_Metal_DestroyView(this->metal_view);
		this->metal_view = nullptr;
#endif
		SDL_DestroyWindow(this->sdl_window);
		this->sdl_window = nullptr;
		return "wgpu: WgpuDevice::Init() failed";
	}

	/* Expose the global device pointer. */
	_wgpu_device = &this->gpu_device;

	/* Create the sprite atlas now that the GPU device is ready. */
	static SpriteAtlas atlas_instance;
	_sprite_atlas = &atlas_instance;

	/* Temporary atlas smoke-test: upload a 64×64 red square. */
	{
		std::vector<uint8_t> test_rgba(64 * 64 * 4, 0);
		for (size_t i = 0; i < test_rgba.size(); i += 4) {
			test_rgba[i]     = 255; /* R */
			test_rgba[i + 3] = 255; /* A */
		}
		AtlasEntry e = _sprite_atlas->Upload(0, test_rgba.data(), nullptr, 64, 64, 0, 0, ZoomLevel::Normal);
		Debug(driver, 0, "atlas test: page={} uv=({:.5f},{:.5f})..({:.5f},{:.5f}) valid={}",
			e.page, e.u0, e.v0, e.u1, e.v1, e.valid ? 1 : 0);
	}

	/* Allocate CPU-side pixel buffer for the blitter / UI layer. */
	this->video_buffer.assign(static_cast<size_t>(w) * h, 0);
	this->anim_buffer.assign(static_cast<size_t>(w) * h, 0);
	_screen.dst_ptr = this->video_buffer.data();
	_screen.width   = static_cast<int>(w);
	_screen.height  = static_cast<int>(h);
	_screen.pitch   = static_cast<int>(w);

	/* Create GPU blit resources (intermediate texture + fullscreen pipeline). */
	this->CreateBlitResources(static_cast<int>(w), static_cast<int>(h));

	/* Initialise the GPU renderer (sprite pipeline + UI composite). */
	if (!this->renderer.Init(&this->gpu_device)) {
		Debug(driver, 0, "wgpu: GpuRenderer::Init() failed (non-fatal, UI blit still works)");
	}

	/* Expose the global command buffer pointer for viewport hooks. */
	_gpu_command_buffer = &this->command_buffer;

	this->driver_info = "wgpu (WebGPU native)";

	SDL_StopTextInput();
	this->edit_box_focused = false;
	this->is_game_threaded = !GetDriverParamBool(param, "no_threads") && !GetDriverParamBool(param, "no_thread");

	/* Initialize blitter's internal buffers (e.g. anim_buf for 32bpp-anim). */
	BlitterFactory::GetCurrentBlitter()->PostResize();

	/* Initialize dirty block array before marking screen dirty. */
	ScreenSizeChanged();
	MarkWholeScreenDirty();

	return std::nullopt;
}

void VideoDriver_Wgpu::Stop()
{
	_gpu_command_buffer = nullptr;
	this->renderer.Shutdown();

	this->DestroyBlitResources();

	_sprite_atlas = nullptr;
	_wgpu_device = nullptr;
	this->gpu_device.Shutdown();

#ifdef __APPLE__
	if (this->metal_view != nullptr) {
		SDL_Metal_DestroyView(this->metal_view);
		this->metal_view = nullptr;
	}
#endif

	if (this->sdl_window != nullptr) {
		SDL_DestroyWindow(this->sdl_window);
		this->sdl_window = nullptr;
	}

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	if (SDL_WasInit(SDL_INIT_EVERYTHING) == 0) SDL_Quit();
}

/* -------------------------------------------------------------------------
 * MainLoop / Paint / RenderFrame
 * -------------------------------------------------------------------------*/

void VideoDriver_Wgpu::MainLoop()
{
	this->StartGameThread();

	while (!_exit_game) {
		this->Tick();
		this->SleepTillNextTick();
	}

	this->StopGameThread();
}

void VideoDriver_Wgpu::RenderFrame()
{
	if (!this->gpu_device.IsReady()) return;

	/* Use the GpuRenderer for the full frame if it's ready. */
	if (this->renderer.BeginFrame()) {
		this->renderer.SubmitSprites(this->command_buffer);
		this->renderer.CompositeUI(this->video_buffer.data(), _screen.width, _screen.height);
		this->renderer.Present();
		this->command_buffer.Reset();
		return;
	}

	/* Fallback: old blit-only path (surface not ready or renderer not initialised). */
	if (this->blit_pipeline == nullptr) return;

	WGPUSurface surface = this->gpu_device.GetSurface();

	/* 1. Acquire next surface texture. */
	WGPUSurfaceTexture surface_texture{};
	wgpuSurfaceGetCurrentTexture(surface, &surface_texture);
	switch (surface_texture.status) {
		case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
			break;

		case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
			Debug(driver, 1, "[wgpu] Surface became suboptimal, reconfiguring");
			this->gpu_device.ReconfigureSurface();
			break;

		case WGPUSurfaceGetCurrentTextureStatus_Timeout:
			Debug(driver, 1, "[wgpu] Surface acquire timed out");
			return;

		case WGPUSurfaceGetCurrentTextureStatus_Outdated:
		case WGPUSurfaceGetCurrentTextureStatus_Lost: {
			int w, h;
			this->GetDrawableSize(w, h);
			Debug(driver, 1, "[wgpu] Surface acquire requires reconfigure, resizing to {}x{}", w, h);
			this->gpu_device.Resize(w, h);
			return;
		}

		case WGPUSurfaceGetCurrentTextureStatus_OutOfMemory:
		case WGPUSurfaceGetCurrentTextureStatus_DeviceLost:
		case WGPUSurfaceGetCurrentTextureStatus_Error:
		default:
			Debug(driver, 0, "[wgpu] Failed to acquire surface texture, status={}", static_cast<int>(surface_texture.status));
			return;
	}

	int w = _screen.width;
	int h = _screen.height;

	/* 2. Upload video_buffer (CPU framebuffer) to the intermediate screen texture. */
	WGPUTexelCopyTextureInfo dst_info{};
	dst_info.texture = this->screen_texture;
	dst_info.mipLevel = 0;
	dst_info.origin = {0, 0, 0};
	dst_info.aspect = WGPUTextureAspect_All;

	WGPUTexelCopyBufferLayout layout{};
	layout.offset = 0;
	layout.bytesPerRow = static_cast<uint32_t>(w) * 4;
	layout.rowsPerImage = static_cast<uint32_t>(h);

	WGPUExtent3D extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};

	wgpuQueueWriteTexture(
		this->gpu_device.GetQueue(),
		&dst_info,
		this->video_buffer.data(),
		static_cast<size_t>(w) * h * 4,
		&layout,
		&extent
	);

	/* 3. Create a texture view for the surface texture (render target). */
	WGPUTextureView surface_view = wgpuTextureCreateView(surface_texture.texture, nullptr);

	/* 4. Encode a render pass that blits the screen texture onto the surface. */
	WGPUCommandEncoderDescriptor enc_desc{};
	enc_desc.nextInChain = nullptr;
	enc_desc.label = {.data = "blit_encoder", .length = WGPU_STRLEN};
	WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(this->gpu_device.GetDevice(), &enc_desc);

	WGPURenderPassColorAttachment color_att{};
	color_att.nextInChain = nullptr;
	color_att.view = surface_view;
	color_att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
	color_att.resolveTarget = nullptr;
	color_att.loadOp = WGPULoadOp_Clear;
	color_att.storeOp = WGPUStoreOp_Store;
	color_att.clearValue = {0.0, 0.0, 0.0, 1.0};

	WGPURenderPassDescriptor rp_desc{};
	rp_desc.nextInChain = nullptr;
	rp_desc.label = {.data = "blit_pass", .length = WGPU_STRLEN};
	rp_desc.colorAttachmentCount = 1;
	rp_desc.colorAttachments = &color_att;
	rp_desc.depthStencilAttachment = nullptr;
	rp_desc.occlusionQuerySet = nullptr;
	rp_desc.timestampWrites = nullptr;

	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp_desc);
	wgpuRenderPassEncoderSetPipeline(pass, this->blit_pipeline);
	wgpuRenderPassEncoderSetBindGroup(pass, 0, this->blit_bind_group, 0, nullptr);
	wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);

	/* 5. Submit and present. */
	WGPUCommandBufferDescriptor cb_desc{};
	cb_desc.nextInChain = nullptr;
	cb_desc.label = {.data = "blit_cmdbuf", .length = WGPU_STRLEN};
	WGPUCommandBuffer cmd_buf = wgpuCommandEncoderFinish(encoder, &cb_desc);
	wgpuQueueSubmit(this->gpu_device.GetQueue(), 1, &cmd_buf);
	wgpuCommandBufferRelease(cmd_buf);
	wgpuCommandEncoderRelease(encoder);

	wgpuTextureViewRelease(surface_view);
	WGPUStatus present_status = wgpuSurfacePresent(surface);
	if (present_status != WGPUStatus_Success) {
		Debug(driver, 0, "[wgpu] Surface present failed, status={}", static_cast<int>(present_status));
	}
	wgpuTextureRelease(surface_texture.texture);
}

void VideoDriver_Wgpu::Paint()
{
	PerformanceMeasurer framerate(PFE_VIDEO);
	this->RenderFrame();
}

/* -------------------------------------------------------------------------
 * MakeDirty / ChangeResolution / ToggleFullscreen
 * -------------------------------------------------------------------------*/

void VideoDriver_Wgpu::MakeDirty(int /*left*/, int /*top*/, int /*width*/, int /*height*/)
{
	/* GPU redraws the entire frame each tick — no dirty-rect tracking needed. */
}

bool VideoDriver_Wgpu::AfterBlitterChange()
{
	/* When the blitter changes (e.g. openttd_main overrides our selection),
	 * the new blitter's internal buffers must be initialized. */
	this->video_buffer.assign(static_cast<size_t>(_screen.width) * _screen.height, 0);
	this->anim_buffer.assign(static_cast<size_t>(_screen.width) * _screen.height, 0);
	_screen.dst_ptr = this->video_buffer.data();
	BlitterFactory::GetCurrentBlitter()->PostResize();
	return true;
}

void VideoDriver_Wgpu::EditBoxGainedFocus()
{
	if (!this->edit_box_focused) {
		SDL_StartTextInput();
		this->edit_box_focused = true;
	}
}

void VideoDriver_Wgpu::EditBoxLostFocus()
{
	if (this->edit_box_focused) {
		SDL_StopTextInput();
		this->edit_box_focused = false;
	}
}

void VideoDriver_Wgpu::ResizeWindow(int w, int h)
{
	if (w < 64) w = 64;
	if (h < 64) h = 64;

	int drawable_w = w;
	int drawable_h = h;
	this->GetDrawableSize(drawable_w, drawable_h);

	this->DestroyBlitResources();
	this->gpu_device.Resize(drawable_w, drawable_h);
	this->renderer.Resize(drawable_w, drawable_h);

	this->video_buffer.assign(static_cast<size_t>(w) * h, 0);
	this->anim_buffer.assign(static_cast<size_t>(w) * h, 0);
	_screen.dst_ptr = this->video_buffer.data();
	_screen.width   = w;
	_screen.height  = h;
	_screen.pitch   = w;

	this->CreateBlitResources(w, h);

	BlitterFactory::GetCurrentBlitter()->PostResize();
	GameSizeChanged();
}

bool VideoDriver_Wgpu::ChangeResolution(int w, int h)
{
	SDL_SetWindowSize(this->sdl_window, w, h);
	this->ResizeWindow(w, h);
	return true;
}

bool VideoDriver_Wgpu::ToggleFullscreen(bool fullscreen)
{
	int ret = SDL_SetWindowFullscreen(this->sdl_window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	if (ret == 0) _fullscreen = fullscreen;
	InvalidateWindowClassesData(WC_GAME_OPTIONS, 3);
	return ret == 0;
}

/* -------------------------------------------------------------------------
 * InputLoop / PollEvent
 * -------------------------------------------------------------------------*/

void VideoDriver_Wgpu::InputLoop()
{
	uint32_t mod = SDL_GetModState();
	const Uint8 *keys = SDL_GetKeyboardState(nullptr);

	bool old_ctrl_pressed = _ctrl_pressed;

	_ctrl_pressed  = !!(mod & KMOD_CTRL);
	_shift_pressed = !!(mod & KMOD_SHIFT);

	this->fast_forward_key_pressed = keys[SDL_SCANCODE_TAB] && (mod & KMOD_ALT) == 0;

	_dirkeys =
		(keys[SDL_SCANCODE_LEFT]  ? 1 : 0) |
		(keys[SDL_SCANCODE_UP]    ? 2 : 0) |
		(keys[SDL_SCANCODE_RIGHT] ? 4 : 0) |
		(keys[SDL_SCANCODE_DOWN]  ? 8 : 0);

	if (old_ctrl_pressed != _ctrl_pressed) HandleCtrlChanged();
}

bool VideoDriver_Wgpu::PollEvent()
{
	SDL_Event ev;
	if (!SDL_PollEvent(&ev)) return false;

	switch (ev.type) {
		case SDL_MOUSEMOTION: {
			int32_t x = ev.motion.x;
			int32_t y = ev.motion.y;

			if (_cursor.fix_at) {
				while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEMOTION)) {
					x = ev.motion.x;
					y = ev.motion.y;
				}
			}

			if (_cursor.UpdateCursorPosition(x, y)) {
				SDL_WarpMouseInWindow(this->sdl_window, _cursor.pos.x, _cursor.pos.y);
			}
			HandleMouseEvents();
			break;
		}

		case SDL_MOUSEWHEEL: {
			if (ev.wheel.y > 0) {
				_cursor.wheel--;
			} else if (ev.wheel.y < 0) {
				_cursor.wheel++;
			}

			const float SCROLL_MULTIPLIER = 14.0f;
#if SDL_VERSION_ATLEAST(2, 18, 0)
			_cursor.v_wheel -= ev.wheel.preciseY * SCROLL_MULTIPLIER * _settings_client.gui.scrollwheel_multiplier;
			_cursor.h_wheel += ev.wheel.preciseX * SCROLL_MULTIPLIER * _settings_client.gui.scrollwheel_multiplier;
#else
			_cursor.v_wheel -= static_cast<float>(ev.wheel.y) * SCROLL_MULTIPLIER * _settings_client.gui.scrollwheel_multiplier;
			_cursor.h_wheel += static_cast<float>(ev.wheel.x) * SCROLL_MULTIPLIER * _settings_client.gui.scrollwheel_multiplier;
#endif
			_cursor.wheel_moved = true;
			break;
		}

		case SDL_MOUSEBUTTONDOWN:
			if (_rightclick_emulate && SDL_GetModState() & KMOD_CTRL) {
				ev.button.button = SDL_BUTTON_RIGHT;
			}
			switch (ev.button.button) {
				case SDL_BUTTON_LEFT:
					_left_button_down = true;
					break;
				case SDL_BUTTON_RIGHT:
					_right_button_down = true;
					_right_button_clicked = true;
					break;
				default: break;
			}
			HandleMouseEvents();
			break;

		case SDL_MOUSEBUTTONUP:
			if (_rightclick_emulate) {
				_right_button_down = false;
				_left_button_down = false;
				_left_button_clicked = false;
			} else if (ev.button.button == SDL_BUTTON_LEFT) {
				_left_button_down = false;
				_left_button_clicked = false;
			} else if (ev.button.button == SDL_BUTTON_RIGHT) {
				_right_button_down = false;
			}
			HandleMouseEvents();
			break;

		case SDL_QUIT:
			HandleExitGameRequest();
			break;

		case SDL_KEYDOWN:
			if ((ev.key.keysym.mod & (KMOD_ALT | KMOD_GUI)) &&
					(ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_f)) {
				if (ev.key.repeat == 0) ToggleFullScreen(!_fullscreen);
			} else {
				char32_t character;
				uint keycode = ConvertSdlKeyIntoMy_Wgpu(&ev.key.keysym, &character);
				if (!this->edit_box_focused ||
					keycode == WKC_DELETE ||
					keycode == WKC_NUM_ENTER ||
					keycode == WKC_LEFT ||
					keycode == WKC_RIGHT ||
					keycode == WKC_UP ||
					keycode == WKC_DOWN ||
					keycode == WKC_HOME ||
					keycode == WKC_END ||
					keycode & WKC_META ||
					keycode & WKC_CTRL ||
					keycode & WKC_ALT ||
					(keycode >= WKC_F1 && keycode <= WKC_F12) ||
					!IsValidChar(character, CS_ALPHANUMERAL)) {
					HandleKeypress(keycode, character);
				}
			}
			break;

		case SDL_TEXTINPUT: {
			if (!this->edit_box_focused) break;
			SDL_Keycode kc = SDL_GetKeyFromName(ev.text.text);
			uint keycode = ConvertSdlKeycodeIntoMy_Wgpu(kc);

			if (keycode == WKC_BACKQUOTE && FocusedWindowIsConsole()) {
				auto [len, c] = DecodeUtf8(ev.text.text);
				if (len > 0) HandleKeypress(keycode, c);
			} else {
				HandleTextInput(ev.text.text);
			}
			break;
		}

		case SDL_WINDOWEVENT:
			if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
				int w = std::max(ev.window.data1, 64);
				int h = std::max(ev.window.data2, 64);
				this->ResizeWindow(w, h);
			} else if (ev.window.event == SDL_WINDOWEVENT_ENTER) {
				_cursor.in_window = true;
				SDL_SetRelativeMouseMode(SDL_FALSE);
			} else if (ev.window.event == SDL_WINDOWEVENT_LEAVE) {
				UndrawMouseCursor();
				_cursor.in_window = false;
			}
			break;
	}

	return true;
}

#endif /* WITH_WGPU */

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
	uint32_t flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
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

	/* Allocate CPU-side pixel buffer for the blitter / UI layer. */
	this->video_buffer.assign(static_cast<size_t>(w) * h, 0);
	this->anim_buffer.assign(static_cast<size_t>(w) * h, 0);
	_screen.dst_ptr = this->video_buffer.data();
	_screen.width   = static_cast<int>(w);
	_screen.height  = static_cast<int>(h);
	_screen.pitch   = static_cast<int>(w);

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

	WGPUSurface surface = this->gpu_device.GetSurface();

	/* Acquire next surface texture. */
	WGPUSurfaceTexture surface_texture;
	wgpuSurfaceGetCurrentTexture(surface, &surface_texture);
	if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
		surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) return;

	/* Upload the CPU-rendered framebuffer directly to the surface texture.
	 * The blitter has drawn the game world + UI into video_buffer (BGRA8).
	 * We copy it to the surface texture and present. */
	int w = this->gpu_device.GetWidth();
	int h = this->gpu_device.GetHeight();

	WGPUTexelCopyTextureInfo dst_info = {};
	dst_info.texture = surface_texture.texture;
	dst_info.mipLevel = 0;
	dst_info.origin = {0, 0, 0};

	WGPUTexelCopyBufferLayout layout = {};
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

	wgpuTextureRelease(surface_texture.texture);
	wgpuSurfacePresent(surface);
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

	this->gpu_device.Resize(w, h);

	this->video_buffer.assign(static_cast<size_t>(w) * h, 0);
	this->anim_buffer.assign(static_cast<size_t>(w) * h, 0);
	_screen.dst_ptr = this->video_buffer.data();
	_screen.width   = w;
	_screen.height  = h;
	_screen.pitch   = w;

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

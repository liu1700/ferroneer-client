/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file wgpu_v.h Video driver using wgpu (WebGPU native) for GPU-accelerated rendering. */

#ifndef VIDEO_WGPU_V_H
#define VIDEO_WGPU_V_H

#ifdef WITH_WGPU

#include "video_driver.hpp"
#include "../gpu/wgpu_device.h"
#include <vector>

struct SDL_Window;

/** Video driver backed by wgpu for GPU rendering, using SDL2 for windowing. */
class VideoDriver_Wgpu : public VideoDriver {
public:
	VideoDriver_Wgpu() : VideoDriver(true) {}

	std::optional<std::string_view> Start(const StringList &param) override;
	void Stop() override;

	void MakeDirty(int left, int top, int width, int height) override;
	void MainLoop() override;

	bool ChangeResolution(int w, int h) override;
	bool ToggleFullscreen(bool fullscreen) override;

	std::string_view GetName() const override { return "wgpu"; }
	std::string_view GetInfoString() const override { return this->driver_info; }

protected:
	void InputLoop() override;
	bool PollEvent() override;

private:
	void Paint();
	void RenderFrame();
	void ResizeWindow(int w, int h);

	SDL_Window *sdl_window = nullptr;         ///< SDL2 window handle.
	void *metal_view = nullptr;               ///< SDL_MetalView (macOS only).
	WgpuDevice gpu_device;                    ///< Owned wgpu device/surface.

	std::vector<uint32_t> video_buffer;       ///< CPU-side pixel buffer for blitter/UI.
	std::string driver_info;                  ///< Human-readable driver info string.

	bool edit_box_focused = false;            ///< Whether SDL text input mode is active.
};

#endif /* WITH_WGPU */

#endif /* VIDEO_WGPU_V_H */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file wgpu_device.h Wrapper for wgpu instance, adapter, device, and surface. */

#ifndef GPU_WGPU_DEVICE_H
#define GPU_WGPU_DEVICE_H

#ifdef WITH_WGPU

#include <webgpu/webgpu.h>

/** Owns the wgpu device and surface. Created once, shared by video driver and atlas. */
class WgpuDevice {
public:
	WgpuDevice();
	~WgpuDevice();

	WgpuDevice(const WgpuDevice &) = delete;
	WgpuDevice &operator=(const WgpuDevice &) = delete;

	bool Init(void *native_layer, int width, int height);
	void Resize(int width, int height);
	void Shutdown();

	bool IsReady() const { return this->device != nullptr; }

	WGPUDevice GetDevice() const { return this->device; }
	WGPUQueue GetQueue() const { return this->queue; }
	WGPUSurface GetSurface() const { return this->surface; }
	WGPUTextureFormat GetSurfaceFormat() const { return this->surface_format; }
	int GetWidth() const { return this->width; }
	int GetHeight() const { return this->height; }

private:
	WGPUInstance instance = nullptr;
	WGPUAdapter adapter = nullptr;
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	WGPUSurface surface = nullptr;
	WGPUTextureFormat surface_format = WGPUTextureFormat_BGRA8Unorm;
	int width = 0;
	int height = 0;

	WGPUSurface CreateSurfaceFromNativeLayer(void *native_layer);
};

extern WgpuDevice *_wgpu_device;

#endif /* WITH_WGPU */

#endif /* GPU_WGPU_DEVICE_H */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file wgpu_device.cpp wgpu instance, adapter, device, and surface initialisation. */

#ifdef WITH_WGPU

#include "wgpu_device.h"

#include <cstdio>

static std::string_view WGPUStringViewToStringView(WGPUStringView view)
{
	if (view.data == nullptr) return {};
	if (view.length == WGPU_STRLEN) return view.data;
	return std::string_view(view.data, view.length);
}

static bool ContainsTextureFormat(const WGPUSurfaceCapabilities &capabilities, WGPUTextureFormat format)
{
	for (size_t i = 0; i < capabilities.formatCount; i++) {
		if (capabilities.formats[i] == format) return true;
	}
	return false;
}

static bool ContainsPresentMode(const WGPUSurfaceCapabilities &capabilities, WGPUPresentMode mode)
{
	for (size_t i = 0; i < capabilities.presentModeCount; i++) {
		if (capabilities.presentModes[i] == mode) return true;
	}
	return false;
}

static bool ContainsAlphaMode(const WGPUSurfaceCapabilities &capabilities, WGPUCompositeAlphaMode mode)
{
	for (size_t i = 0; i < capabilities.alphaModeCount; i++) {
		if (capabilities.alphaModes[i] == mode) return true;
	}
	return false;
}

WgpuDevice *_wgpu_device = nullptr;

WgpuDevice::WgpuDevice() = default;

WgpuDevice::~WgpuDevice()
{
	this->Shutdown();
}

bool WgpuDevice::Init(void *native_layer, int width, int height)
{
	this->width = width;
	this->height = height;

	/* Create instance. */
	WGPUInstanceDescriptor inst_desc{};
	inst_desc.nextInChain = nullptr;
	this->instance = wgpuCreateInstance(&inst_desc);
	if (this->instance == nullptr) {
		fprintf(stderr, "[wgpu] Failed to create WGPUInstance\n");
		return false;
	}

	/* Create surface from platform-specific native layer (e.g. CAMetalLayer on macOS). */
	this->surface = this->CreateSurfaceFromNativeLayer(native_layer);
	if (this->surface == nullptr) {
		fprintf(stderr, "[wgpu] Failed to create WGPUSurface\n");
		return false;
	}

	/* Request adapter — synchronised via WaitAny. */
	struct AdapterResult {
		WGPUAdapter adapter = nullptr;
		bool done = false;
	} adapter_result;

	WGPURequestAdapterOptions adapter_opts{};
	adapter_opts.nextInChain = nullptr;
	adapter_opts.powerPreference = WGPUPowerPreference_HighPerformance;
	adapter_opts.compatibleSurface = this->surface;

	WGPURequestAdapterCallbackInfo adapter_cb{};
	adapter_cb.nextInChain = nullptr;
	adapter_cb.mode = WGPUCallbackMode_AllowSpontaneous;
	adapter_cb.userdata1 = &adapter_result;
	adapter_cb.userdata2 = nullptr;
	adapter_cb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter,
	                          WGPUStringView /*message*/, void *userdata1, void * /*userdata2*/) {
		AdapterResult *result = static_cast<AdapterResult *>(userdata1);
		if (status == WGPURequestAdapterStatus_Success) {
			result->adapter = adapter;
		} else {
			fprintf(stderr, "[wgpu] Adapter request failed (status %d)\n", static_cast<int>(status));
		}
		result->done = true;
	};

	/* wgpu-native fires the callback synchronously with AllowSpontaneous. */
	wgpuInstanceRequestAdapter(this->instance, &adapter_opts, adapter_cb);

	if (adapter_result.adapter == nullptr) {
		fprintf(stderr, "[wgpu] No suitable adapter found\n");
		return false;
	}
	this->adapter = adapter_result.adapter;

	/* Request device — synchronised via WaitAny. */
	struct DeviceResult {
		WGPUDevice device = nullptr;
		bool done = false;
	} device_result;

	WGPUDeviceDescriptor device_desc{};
	device_desc.nextInChain = nullptr;
	device_desc.label = {.data = "openttd_wgpu_device", .length = WGPU_STRLEN};
	/* Leave limits/features at defaults (request nothing extra). */
	device_desc.defaultQueue.label = {.data = "openttd_wgpu_queue", .length = WGPU_STRLEN};
	device_desc.deviceLostCallbackInfo = {
		.nextInChain = nullptr,
		.mode = WGPUCallbackMode_AllowSpontaneous,
		.callback = [](WGPUDevice const * /*device*/, WGPUDeviceLostReason reason, WGPUStringView message, void * /*userdata1*/, void * /*userdata2*/) {
			fprintf(stderr, "[wgpu] Device lost (reason=%d): %.*s\n", static_cast<int>(reason), static_cast<int>(WGPUStringViewToStringView(message).size()), WGPUStringViewToStringView(message).data());
		},
		.userdata1 = nullptr,
		.userdata2 = nullptr,
	};
	device_desc.uncapturedErrorCallbackInfo = {
		.nextInChain = nullptr,
		.callback = [](WGPUDevice const * /*device*/, WGPUErrorType type, WGPUStringView message, void * /*userdata1*/, void * /*userdata2*/) {
			fprintf(stderr, "[wgpu] Uncaptured error (type=%d): %.*s\n", static_cast<int>(type), static_cast<int>(WGPUStringViewToStringView(message).size()), WGPUStringViewToStringView(message).data());
		},
		.userdata1 = nullptr,
		.userdata2 = nullptr,
	};

	WGPURequestDeviceCallbackInfo device_cb{};
	device_cb.nextInChain = nullptr;
	device_cb.mode = WGPUCallbackMode_AllowSpontaneous;
	device_cb.userdata1 = &device_result;
	device_cb.userdata2 = nullptr;
	device_cb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device,
	                         WGPUStringView /*message*/, void *userdata1, void * /*userdata2*/) {
		DeviceResult *result = static_cast<DeviceResult *>(userdata1);
		if (status == WGPURequestDeviceStatus_Success) {
			result->device = device;
		} else {
			fprintf(stderr, "[wgpu] Device request failed (status %d)\n", static_cast<int>(status));
		}
		result->done = true;
	};

	/* wgpu-native fires the callback synchronously with AllowSpontaneous. */
	wgpuAdapterRequestDevice(this->adapter, &device_desc, device_cb);

	if (device_result.device == nullptr) {
		fprintf(stderr, "[wgpu] Failed to create WGPUDevice\n");
		return false;
	}
	this->device = device_result.device;

	/* Get queue. */
	this->queue = wgpuDeviceGetQueue(this->device);
	if (this->queue == nullptr) {
		fprintf(stderr, "[wgpu] Failed to get WGPUQueue\n");
		return false;
	}

	if (!this->SelectSurfaceConfiguration()) return false;
	if (!this->ReconfigureSurface()) return false;

	fprintf(stdout, "[wgpu] Device initialised (%dx%d)\n", width, height);
	return true;
}

void WgpuDevice::Resize(int width, int height)
{
	if (this->device == nullptr || this->surface == nullptr) return;
	this->width = width;
	this->height = height;
	this->ReconfigureSurface();
}

bool WgpuDevice::ReconfigureSurface()
{
	if (this->device == nullptr || this->surface == nullptr) return false;
	if (this->width <= 0 || this->height <= 0) return false;

	WGPUSurfaceConfiguration surf_cfg{};
	surf_cfg.nextInChain = nullptr;
	surf_cfg.device = this->device;
	surf_cfg.format = this->surface_format;
	surf_cfg.usage = WGPUTextureUsage_RenderAttachment;
	surf_cfg.width = static_cast<uint32_t>(this->width);
	surf_cfg.height = static_cast<uint32_t>(this->height);
	surf_cfg.viewFormatCount = 0;
	surf_cfg.viewFormats = nullptr;
	surf_cfg.alphaMode = this->alpha_mode;
	surf_cfg.presentMode = this->present_mode;
	wgpuSurfaceConfigure(this->surface, &surf_cfg);
	return true;
}

bool WgpuDevice::SelectSurfaceConfiguration()
{
	WGPUSurfaceCapabilities capabilities{};
	if (wgpuSurfaceGetCapabilities(this->surface, this->adapter, &capabilities) != WGPUStatus_Success) {
		fprintf(stderr, "[wgpu] Failed to query surface capabilities\n");
		return false;
	}

	if (capabilities.formatCount == 0 || capabilities.presentModeCount == 0 || capabilities.alphaModeCount == 0) {
		fprintf(stderr, "[wgpu] Surface capabilities are incomplete\n");
		wgpuSurfaceCapabilitiesFreeMembers(capabilities);
		return false;
	}

	this->surface_format = ContainsTextureFormat(capabilities, WGPUTextureFormat_BGRA8Unorm) ? WGPUTextureFormat_BGRA8Unorm : capabilities.formats[0];
	this->present_mode = ContainsPresentMode(capabilities, WGPUPresentMode_Fifo) ? WGPUPresentMode_Fifo : capabilities.presentModes[0];
	this->alpha_mode = ContainsAlphaMode(capabilities, WGPUCompositeAlphaMode_Opaque) ? WGPUCompositeAlphaMode_Opaque : capabilities.alphaModes[0];

	fprintf(stdout, "[wgpu] Surface configured with format=%d presentMode=%d alphaMode=%d\n",
		static_cast<int>(this->surface_format), static_cast<int>(this->present_mode), static_cast<int>(this->alpha_mode));
	wgpuSurfaceCapabilitiesFreeMembers(capabilities);
	return true;
}

void WgpuDevice::Shutdown()
{
	if (this->surface != nullptr) {
		wgpuSurfaceUnconfigure(this->surface);
	}
	if (this->queue != nullptr) {
		wgpuQueueRelease(this->queue);
		this->queue = nullptr;
	}
	if (this->device != nullptr) {
		wgpuDeviceRelease(this->device);
		this->device = nullptr;
	}
	if (this->surface != nullptr) {
		wgpuSurfaceRelease(this->surface);
		this->surface = nullptr;
	}
	if (this->adapter != nullptr) {
		wgpuAdapterRelease(this->adapter);
		this->adapter = nullptr;
	}
	if (this->instance != nullptr) {
		wgpuInstanceRelease(this->instance);
		this->instance = nullptr;
	}
}

WGPUSurface WgpuDevice::CreateSurfaceFromNativeLayer(void *native_layer)
{
#ifdef __APPLE__
	/* On macOS, native_layer is a CAMetalLayer* obtained by the caller via SDL_Metal_GetLayer(). */
	if (native_layer == nullptr) {
		fprintf(stderr, "[wgpu] CreateSurfaceFromNativeLayer: null CAMetalLayer\n");
		return nullptr;
	}

	WGPUSurfaceSourceMetalLayer metal_src{};
	metal_src.chain.next = nullptr;
	metal_src.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
	metal_src.layer = native_layer;

	WGPUSurfaceDescriptor surf_desc{};
	surf_desc.nextInChain = &metal_src.chain;
	surf_desc.label = WGPU_STRING_VIEW_INIT;

	return wgpuInstanceCreateSurface(this->instance, &surf_desc);
#else
	/* Unsupported platform. */
	(void)native_layer;
	fprintf(stderr, "[wgpu] CreateSurfaceFromNativeLayer: unsupported platform\n");
	return nullptr;
#endif
}

#endif /* WITH_WGPU */

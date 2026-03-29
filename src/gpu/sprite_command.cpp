/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file sprite_command.cpp GPU sprite command buffer management. */

#include "../stdafx.h"

#ifdef WITH_WGPU

#include "sprite_command.h"

#include "../debug.h"

#include "../safeguards.h"

SpriteCommandBuffer *_gpu_command_buffer = nullptr;
bool _gpu_suppress_sprite_emit = false;

void SpriteCommandBuffer::Reset()
{
	for (auto &batch : this->batches) batch.Reset();
}

void SpriteCommandBuffer::Emit(uint16_t atlas_page, const GpuSpriteInstance &instance)
{
	if (atlas_page >= MAX_ATLAS_PAGES) {
		static uint32_t dropped_page_logs = 0;
		if (dropped_page_logs < 16) {
			Debug(driver, 0, "[gpu] dropping sprite on atlas page {} (MAX_ATLAS_PAGES={})",
				atlas_page, static_cast<uint32_t>(MAX_ATLAS_PAGES));
			dropped_page_logs++;
		}
		return;
	}
	this->batches[atlas_page].instances.push_back(instance);
}

size_t SpriteCommandBuffer::PageCount(uint16_t page) const
{
	if (page >= MAX_ATLAS_PAGES) return 0;
	return this->batches[page].instances.size();
}

const GpuSpriteInstance *SpriteCommandBuffer::PageData(uint16_t page) const
{
	if (page >= MAX_ATLAS_PAGES) return nullptr;
	return this->batches[page].instances.data();
}

size_t SpriteCommandBuffer::TotalCount() const
{
	size_t total = 0;
	for (const auto &batch : this->batches) total += batch.instances.size();
	return total;
}

uint16_t SpriteCommandBuffer::MaxActivePage() const
{
	for (int i = MAX_ATLAS_PAGES - 1; i >= 0; i--) {
		if (!this->batches[i].instances.empty()) return static_cast<uint16_t>(i);
	}
	return 0;
}

#endif /* WITH_WGPU */

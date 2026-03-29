/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file wgpu_default_v.h Factory registration for the wgpu video driver. */

#ifndef VIDEO_WGPU_DEFAULT_V_H
#define VIDEO_WGPU_DEFAULT_V_H

#ifdef WITH_WGPU

#include "wgpu_v.h"
#include "../driver.h"

/** Factory for the wgpu video driver. */
class FVideoDriver_Wgpu : public DriverFactoryBase {
public:
	/* Keep wgpu available for explicit opt-in, but avoid auto-probing it ahead of the
	 * mature macOS backends until surface/present handling is production ready. */
	FVideoDriver_Wgpu() : DriverFactoryBase(Driver::Type::Video, 7, "wgpu", "wgpu GPU-accelerated Video Driver") {}
	std::unique_ptr<Driver> CreateInstance() const override { return std::make_unique<VideoDriver_Wgpu>(); }
};

#endif /* WITH_WGPU */

#endif /* VIDEO_WGPU_DEFAULT_V_H */

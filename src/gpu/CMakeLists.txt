add_files(
    gpu_types.h
    wgpu_device.h
    wgpu_device.cpp
    sprite_atlas.h
    sprite_atlas.cpp
    CONDITION WGPU_FOUND
)

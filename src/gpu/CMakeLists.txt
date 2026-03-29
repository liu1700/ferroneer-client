add_files(
    gpu_types.h
    wgpu_device.h
    wgpu_device.cpp
    sprite_atlas.h
    sprite_atlas.cpp
    sprite_command.h
    sprite_command.cpp
    gpu_renderer.h
    gpu_renderer.cpp
    remap_table.h
    remap_table.cpp
    CONDITION WGPU_FOUND
)

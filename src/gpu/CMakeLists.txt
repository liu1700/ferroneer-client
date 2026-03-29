add_files(
    gpu_types.h
    wgpu_device.h
    wgpu_device.cpp
    CONDITION WGPU_FOUND
)

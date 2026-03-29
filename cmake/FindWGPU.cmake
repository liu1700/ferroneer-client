# FindWGPU.cmake — Locate wgpu-native pre-built library.
set(WGPU_ROOT "${CMAKE_SOURCE_DIR}/deps/wgpu-native" CACHE PATH "Path to wgpu-native")

find_path(WGPU_INCLUDE_DIR
    NAMES webgpu/webgpu.h webgpu/wgpu.h
    PATHS "${WGPU_ROOT}/include"
    NO_DEFAULT_PATH
)

find_library(WGPU_LIBRARY
    NAMES wgpu_native
    PATHS "${WGPU_ROOT}/lib"
    NO_DEFAULT_PATH
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WGPU DEFAULT_MSG WGPU_LIBRARY WGPU_INCLUDE_DIR)

if(WGPU_FOUND)
    set(WGPU_INCLUDE_DIRS "${WGPU_INCLUDE_DIR}")
    set(WGPU_LIBRARIES "${WGPU_LIBRARY}")

    if(NOT TARGET wgpu::wgpu)
        add_library(wgpu::wgpu STATIC IMPORTED)
        set_target_properties(wgpu::wgpu PROPERTIES
            IMPORTED_LOCATION "${WGPU_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${WGPU_INCLUDE_DIR}"
        )
        if(APPLE)
            target_link_libraries(wgpu::wgpu INTERFACE
                "-framework Metal"
                "-framework MetalKit"
                "-framework QuartzCore"
                "-framework CoreFoundation"
                "-framework IOKit"
                "-framework IOSurface"
            )
        elseif(UNIX)
            target_link_libraries(wgpu::wgpu INTERFACE dl pthread m)
        endif()
    endif()
endif()

mark_as_advanced(WGPU_INCLUDE_DIR WGPU_LIBRARY)

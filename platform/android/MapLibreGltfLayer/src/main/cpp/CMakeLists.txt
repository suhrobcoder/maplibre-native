cmake_minimum_required(VERSION 3.18.1)

project(maplibre-gltf-layer LANGUAGES CXX)

# Repo root, six levels up from this file
get_filename_component(MLN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

add_library(maplibre-gltf-layer SHARED
    gltf_loader.cpp
    gltf_model_jni.cpp
)

# Header-only dependency on mbgl: we only include the three custom-layer
# headers (pure-virtual host + POD parameter structs). No mbgl symbols are
# linked; all projection/matrix math is reimplemented inline in the host.
target_include_directories(maplibre-gltf-layer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MLN_ROOT}/include
)

target_compile_features(maplibre-gltf-layer PRIVATE cxx_std_17)

target_link_libraries(maplibre-gltf-layer PRIVATE
    log
)

if(MLN_WITH_VULKAN)
    # --- Vulkan backend (Phase 5) ---
    target_compile_definitions(maplibre-gltf-layer PRIVATE
        MLN_RENDER_BACKEND_VULKAN=1
    )

    target_include_directories(maplibre-gltf-layer PRIVATE
        ${MLN_ROOT}/vendor/vulkan-headers/include
    )

    target_sources(maplibre-gltf-layer PRIVATE
        vulkan/gltf_vulkan_host.cpp
    )
else()
    # --- OpenGL ES backend (default) ---
    target_sources(maplibre-gltf-layer PRIVATE
        gltf_layer_host.cpp
        gltf_model_host.cpp
    )

    target_link_libraries(maplibre-gltf-layer PRIVATE
        GLESv3
    )
endif()

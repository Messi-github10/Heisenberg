# Volk — Vulkan meta-loader, pre-built from vcpkg (MIT licensed)
# Static library, no DLL required
add_library(volk INTERFACE)

target_include_directories(volk INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/include"
)

target_link_libraries(volk INTERFACE
    "$<IF:$<CONFIG:Debug>,${CMAKE_CURRENT_LIST_DIR}/debug/lib/volk.lib,${CMAKE_CURRENT_LIST_DIR}/lib/volk.lib>"
)

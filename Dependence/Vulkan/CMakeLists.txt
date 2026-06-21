# Vulkan — pre-built Windows binaries from vcpkg (Apache 2.0 licensed)
add_library(vulkan INTERFACE)

target_include_directories(vulkan INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/windows/include"
)

target_link_libraries(vulkan INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/windows/lib/vulkan-1.lib
)

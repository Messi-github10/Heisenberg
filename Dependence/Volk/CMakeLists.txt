add_library(volk STATIC
    "${CMAKE_CURRENT_LIST_DIR}/include/volk.c")

target_include_directories(volk PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include")

target_link_libraries(volk PUBLIC vulkan)

target_compile_definitions(volk PUBLIC
    VK_NO_PROTOTYPES)

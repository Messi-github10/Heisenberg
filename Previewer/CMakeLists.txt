file(GLOB_RECURSE PREVIEWER_SOURCES *.cpp *.hpp)

add_library(HeisenbergPreviewer STATIC
    ${PREVIEWER_SOURCES}
)

set_target_properties(HeisenbergPreviewer PROPERTIES
    AUTOMOC ON
)

target_include_directories(HeisenbergPreviewer PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(HeisenbergPreviewer PUBLIC
    ffmpeg
    HeisenbergUtiles
    libplacebo
    miniaudio
    vulkan
    volk
    Qt6::Core
)

target_compile_definitions(HeisenbergPreviewer PUBLIC
    VK_NO_PROTOTYPES
    VK_USE_PLATFORM_WIN32_KHR
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(HeisenbergPreviewer PRIVATE /utf-8)
endif()

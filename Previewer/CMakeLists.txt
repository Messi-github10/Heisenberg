file(GLOB_RECURSE PREVIEWER_SOURCES CONFIGURE_DEPENDS *.cpp *.hpp)

set(VULKAN_SDK_PATH "D:/Environment/VulkanSDK" CACHE PATH
    "Path to the Vulkan SDK used to compile filter shaders")
find_program(GLSLANG_VALIDATOR
    NAMES glslangValidator
    HINTS "$ENV{VULKAN_SDK}/Bin" "${VULKAN_SDK_PATH}/Bin"
    REQUIRED
)
find_program(SPIRV_VAL
    NAMES spirv-val
    HINTS "$ENV{VULKAN_SDK}/Bin" "${VULKAN_SDK_PATH}/Bin"
    REQUIRED
)

set(FILTER_SHADER_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/FilterGraph/Vulkan/Shaders")
set(FILTER_SHADER_BINARY_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/FilterGraph/Vulkan/Shaders")
set(COLOR_INVERT_SHADER
    "${FILTER_SHADER_BINARY_DIR}/colorInvert.comp.spv")

add_custom_command(
    OUTPUT "${COLOR_INVERT_SHADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FILTER_SHADER_BINARY_DIR}"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2
            "${FILTER_SHADER_SOURCE_DIR}/colorInvert.comp"
            -o "${COLOR_INVERT_SHADER}"
    COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${COLOR_INVERT_SHADER}"
    DEPENDS "${FILTER_SHADER_SOURCE_DIR}/colorInvert.comp"
    COMMENT "Compiling Vulkan filter shader: colorInvert.comp"
    VERBATIM
)

add_custom_target(HeisenbergFilterShaders DEPENDS "${COLOR_INVERT_SHADER}")

add_library(HeisenbergPreviewer STATIC
    ${PREVIEWER_SOURCES}
)

add_dependencies(HeisenbergPreviewer HeisenbergFilterShaders)

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

target_compile_definitions(HeisenbergPreviewer PRIVATE
    HEISENBERG_COLOR_INVERT_SHADER_PATH="${COLOR_INVERT_SHADER}"
)

if(MSVC)
    target_compile_options(HeisenbergPreviewer PRIVATE /utf-8)
endif()

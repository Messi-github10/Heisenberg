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
set(TEST_FILTER_GRAPH
    "${CMAKE_CURRENT_SOURCE_DIR}/FilterGraph/Vulkan/Graph/TestGraphs/DAG_FIlter_Graph_Test.json")

function(add_filter_shader output_variable source_name)
    set(shader_source "${FILTER_SHADER_SOURCE_DIR}/${source_name}")
    set(shader_output "${FILTER_SHADER_BINARY_DIR}/${source_name}.spv")
    add_custom_command(
        OUTPUT "${shader_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${FILTER_SHADER_BINARY_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2
                "${shader_source}" -o "${shader_output}"
        COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${shader_output}"
        DEPENDS "${shader_source}"
        COMMENT "Compiling Vulkan filter shader: ${source_name}"
        VERBATIM
    )
    set(${output_variable} "${shader_output}" PARENT_SCOPE)
endfunction()

add_filter_shader(COLOR_INVERT_SHADER colorInvert.comp)
add_filter_shader(EXPOSURE_SHADER exposure.comp)
add_filter_shader(BLEND_SHADER blend.comp)
add_filter_shader(GAUSSIAN_BLUR_SHADER gaussianBlur.comp)

add_custom_target(HeisenbergFilterShaders DEPENDS
    "${COLOR_INVERT_SHADER}"
    "${EXPOSURE_SHADER}"
    "${BLEND_SHADER}"
    "${GAUSSIAN_BLUR_SHADER}"
)

add_library(HeisenbergPreviewer STATIC
    ${PREVIEWER_SOURCES}
)

add_dependencies(HeisenbergPreviewer HeisenbergFilterShaders)

set_target_properties(HeisenbergPreviewer PROPERTIES
    AUTOMOC ON
)

target_include_directories(HeisenbergPreviewer PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Vulkan
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Vulkan/Filters
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Vulkan/Graph
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Vulkan/Nodes
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Vulkan/IOLayer
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Vulkan/Resources
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
    HEISENBERG_EXPOSURE_SHADER_PATH="${EXPOSURE_SHADER}"
    HEISENBERG_BLEND_SHADER_PATH="${BLEND_SHADER}"
    HEISENBERG_GAUSSIAN_BLUR_SHADER_PATH="${GAUSSIAN_BLUR_SHADER}"
    HEISENBERG_TEST_FILTER_GRAPH_PATH="${TEST_FILTER_GRAPH}"
)

if(MSVC)
    target_compile_options(HeisenbergPreviewer PRIVATE /utf-8)
endif()

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
find_program(SPIRV_CROSS
    NAMES spirv-cross
    HINTS "$ENV{VULKAN_SDK}/Bin" "${VULKAN_SDK_PATH}/Bin"
    REQUIRED
)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(FILTER_SHADER_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/FilterGraph/Vulkan/Shaders")
set(FILTER_SHADER_BINARY_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/FilterGraph/Vulkan/Shaders")
set(SHADER_CONTRACT_MANIFEST
    "${FILTER_SHADER_SOURCE_DIR}/shader_manifest.json")
set(SHADER_CONTRACT_VALIDATOR
    "${CMAKE_CURRENT_SOURCE_DIR}/FilterGraph/Vulkan/validate_shader_contract.py")
set(FILTER_CONTRACT_TEST
    "${CMAKE_CURRENT_SOURCE_DIR}/FilterGraph/Vulkan/shader_source_contract_test.py")

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

set(FILTER_SHADER_SPECS
    "COLOR_INVERT|colorInvert.comp"
    "EXPOSURE|exposure.comp"
    "BLEND|blend.comp"
    "GAUSSIAN_BLUR|gaussianBlur.comp"
    "RESIZE|resize.comp"
    "LUT|lut.comp"
    "HISTOGRAM|histogram.comp"
)

set(FILTER_SHADER_OUTPUTS)
set(FILTER_SHADER_DEFINITIONS)
foreach(shader_spec IN LISTS FILTER_SHADER_SPECS)
    string(REPLACE "|" ";" shader_parts "${shader_spec}")
    list(GET shader_parts 0 shader_id)
    list(GET shader_parts 1 shader_source_name)
    set(shader_variable "FILTER_SHADER_${shader_id}")
    add_filter_shader(${shader_variable} "${shader_source_name}")
    list(APPEND FILTER_SHADER_OUTPUTS "${${shader_variable}}")
    list(APPEND FILTER_SHADER_DEFINITIONS
        "HEISENBERG_${shader_id}_SHADER_PATH=\"${${shader_variable}}\"")
endforeach()

add_custom_target(HeisenbergFilterShaders DEPENDS
    ${FILTER_SHADER_OUTPUTS}
)

set(SHADER_CONTRACT_STAMP
    "${FILTER_SHADER_BINARY_DIR}/shader_contract.stamp")
add_custom_command(
    OUTPUT "${SHADER_CONTRACT_STAMP}"
    COMMAND "${Python3_EXECUTABLE}" "${SHADER_CONTRACT_VALIDATOR}"
            --spirv-cross "${SPIRV_CROSS}"
            --manifest "${SHADER_CONTRACT_MANIFEST}"
            --shader-dir "${FILTER_SHADER_BINARY_DIR}"
            --stamp "${SHADER_CONTRACT_STAMP}"
    DEPENDS ${FILTER_SHADER_OUTPUTS}
            "${SHADER_CONTRACT_MANIFEST}"
            "${SHADER_CONTRACT_VALIDATOR}"
    COMMENT "Validating Vulkan Shader ABI contracts"
    VERBATIM
)
add_custom_target(HeisenbergShaderContractTests
    DEPENDS "${SHADER_CONTRACT_STAMP}")

set(FILTER_CONTRACT_STAMP
    "${FILTER_SHADER_BINARY_DIR}/filter_contract.stamp")
add_custom_command(
    OUTPUT "${FILTER_CONTRACT_STAMP}"
    COMMAND "${Python3_EXECUTABLE}" "${FILTER_CONTRACT_TEST}"
            --shader-dir "${FILTER_SHADER_SOURCE_DIR}"
            --manifest "${SHADER_CONTRACT_MANIFEST}"
            --stamp "${FILTER_CONTRACT_STAMP}"
    DEPENDS ${FILTER_SHADER_OUTPUTS}
            "${SHADER_CONTRACT_MANIFEST}"
            "${FILTER_CONTRACT_TEST}"
    COMMENT "Checking Vulkan filter source contracts"
    VERBATIM
)
add_custom_target(HeisenbergShaderSourceContractTests
    DEPENDS "${FILTER_CONTRACT_STAMP}")

add_library(HeisenbergPreviewer STATIC
    ${PREVIEWER_SOURCES}
)

add_dependencies(HeisenbergPreviewer HeisenbergFilterShaders)
add_dependencies(HeisenbergPreviewer HeisenbergShaderContractTests)
add_dependencies(HeisenbergPreviewer HeisenbergShaderSourceContractTests)

set_target_properties(HeisenbergPreviewer PROPERTIES
    AUTOMOC ON
)

target_include_directories(HeisenbergPreviewer PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Common
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Vulkan
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Vulkan/Filters
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Vulkan/Graph
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Vulkan/Nodes
    ${CMAKE_CURRENT_LIST_DIR}/FilterGraph/Vulkan/IONodes
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
    ${FILTER_SHADER_DEFINITIONS}
)

if(MSVC)
    target_compile_options(HeisenbergPreviewer PRIVATE /utf-8)
endif()

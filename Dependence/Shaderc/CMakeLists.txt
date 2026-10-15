set(SHADERC_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/windows")
foreach(filename
    "include/shaderc/shaderc.h"
    "lib/shaderc_shared.lib" "bin/shaderc_shared.dll"
    "debug/lib/shaderc_sharedd.lib" "debug/bin/shaderc_sharedd.dll")
    if(NOT EXISTS "${SHADERC_SDK_DIR}/${filename}")
        message(FATAL_ERROR "Shaderc SDK file is missing: ${filename}. Fetch the Dependence binary files with Git LFS.")
    endif()
endforeach()
add_library(heisenberg_shaderc SHARED IMPORTED GLOBAL)
set_target_properties(heisenberg_shaderc PROPERTIES
    IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
    IMPORTED_IMPLIB "${SHADERC_SDK_DIR}/lib/shaderc_shared.lib"
    IMPORTED_LOCATION "${SHADERC_SDK_DIR}/bin/shaderc_shared.dll"
    IMPORTED_IMPLIB_RELEASE "${SHADERC_SDK_DIR}/lib/shaderc_shared.lib"
    IMPORTED_LOCATION_RELEASE "${SHADERC_SDK_DIR}/bin/shaderc_shared.dll"
    IMPORTED_IMPLIB_DEBUG "${SHADERC_SDK_DIR}/debug/lib/shaderc_sharedd.lib"
    IMPORTED_LOCATION_DEBUG "${SHADERC_SDK_DIR}/debug/bin/shaderc_sharedd.dll"
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE
    MAP_IMPORTED_CONFIG_MINSIZEREL RELEASE
    INTERFACE_INCLUDE_DIRECTORIES "${SHADERC_SDK_DIR}/include"
    INTERFACE_COMPILE_DEFINITIONS SHADERC_SHAREDLIB
    HEISENBERG_RUNTIME_FILES "$<TARGET_FILE:heisenberg_shaderc>"
    HEISENBERG_LICENSE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/licenses"
)
add_library(shaderc::shaderc_shared ALIAS heisenberg_shaderc)

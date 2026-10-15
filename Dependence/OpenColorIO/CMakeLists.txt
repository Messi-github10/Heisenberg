set(OCIO_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/windows")
set(OCIO_RUNTIME_RELEASE
    OpenColorIO_2_5.dll libexpat.dll Imath-3_2.dll yaml-cpp.dll
    z.dll bz2.dll liblzma.dll zstd.dll)
set(OCIO_RUNTIME_DEBUG
    OpenColorIO_2_5.dll libexpatd.dll Imath-3_2_d.dll yaml-cppd.dll
    zd.dll bz2d.dll liblzma.dll zstd.dll)
set(OCIO_RUNTIME_FILES)
foreach(filename IN LISTS OCIO_RUNTIME_RELEASE)
    if(NOT EXISTS "${OCIO_SDK_DIR}/bin/${filename}")
        message(FATAL_ERROR "OpenColorIO SDK runtime file is missing: ${filename}. Fetch the Dependence binary files with Git LFS.")
    endif()
endforeach()
foreach(filename IN LISTS OCIO_RUNTIME_DEBUG)
    if(NOT EXISTS "${OCIO_SDK_DIR}/debug/bin/${filename}")
        message(FATAL_ERROR "OpenColorIO Debug SDK runtime file is missing: ${filename}. Fetch the Dependence binary files with Git LFS.")
    endif()
endforeach()
foreach(index RANGE 0 7)
    list(GET OCIO_RUNTIME_RELEASE ${index} release_file)
    list(GET OCIO_RUNTIME_DEBUG ${index} debug_file)
    list(APPEND OCIO_RUNTIME_FILES "$<IF:$<CONFIG:Debug>,${OCIO_SDK_DIR}/debug/bin/${debug_file},${OCIO_SDK_DIR}/bin/${release_file}>")
endforeach()

add_library(OpenColorIO::OpenColorIO SHARED IMPORTED GLOBAL)
set_target_properties(OpenColorIO::OpenColorIO PROPERTIES
    IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
    IMPORTED_IMPLIB "${OCIO_SDK_DIR}/lib/OpenColorIO.lib"
    IMPORTED_LOCATION "${OCIO_SDK_DIR}/bin/OpenColorIO_2_5.dll"
    IMPORTED_IMPLIB_RELEASE "${OCIO_SDK_DIR}/lib/OpenColorIO.lib"
    IMPORTED_LOCATION_RELEASE "${OCIO_SDK_DIR}/bin/OpenColorIO_2_5.dll"
    IMPORTED_IMPLIB_DEBUG "${OCIO_SDK_DIR}/debug/lib/OpenColorIO.lib"
    IMPORTED_LOCATION_DEBUG "${OCIO_SDK_DIR}/debug/bin/OpenColorIO_2_5.dll"
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE
    MAP_IMPORTED_CONFIG_MINSIZEREL RELEASE
    INTERFACE_INCLUDE_DIRECTORIES "${OCIO_SDK_DIR}/include"
    INTERFACE_COMPILE_FEATURES cxx_std_11
    HEISENBERG_RUNTIME_FILES "${OCIO_RUNTIME_FILES}"
    HEISENBERG_LICENSE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/licenses"
)

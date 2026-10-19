function(heisenberg_deploy_runtime target)
    if(NOT WIN32)
        return()
    endif()
    set(runtime_files
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/debug/bin/spdlogd.dll,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/bin/spdlog.dll>"
        "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avcodec-62.dll"
        "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avdevice-62.dll"
        "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avfilter-11.dll"
        "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avformat-62.dll"
        "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avutil-60.dll"
        "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/swresample-6.dll"
        "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/swscale-9.dll"
        "${CMAKE_SOURCE_DIR}/Dependence/LibPlacebo/windows/bin/libplacebo-365.dll"
        "${CMAKE_SOURCE_DIR}/Dependence/MediaInfo/bin/MediaInfo.dll"
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/Vulkan/windows/debug/bin/vulkan-1.dll,${CMAKE_SOURCE_DIR}/Dependence/Vulkan/windows/bin/vulkan-1.dll>"
        "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/Lcms2/debug/bin/lcms2-2.dll,${CMAKE_SOURCE_DIR}/Dependence/Lcms2/bin/lcms2-2.dll>"
        "$<TARGET_FILE:heisenberg_shaderc>"
    )
    get_target_property(ocio_runtime OpenColorIO::OpenColorIO HEISENBERG_RUNTIME_FILES)
    list(APPEND runtime_files ${ocio_runtime})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses/heisenberg"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/LICENSE"
            "$<TARGET_FILE_DIR:${target}>/licenses/heisenberg/LICENSE"
        VERBATIM)
    foreach(dependency OpenColorIO::OpenColorIO heisenberg_shaderc)
        get_target_property(license_directory ${dependency} HEISENBERG_LICENSE_DIRECTORY)
        if(NOT IS_DIRECTORY "${license_directory}")
            message(FATAL_ERROR "Dependency license directory is missing: ${license_directory}")
        endif()
        get_filename_component(package_directory "${license_directory}" DIRECTORY)
        get_filename_component(package "${package_directory}" NAME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${license_directory}"
                "$<TARGET_FILE_DIR:${target}>/licenses/${package}"
            VERBATIM)
    endforeach()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${runtime_files} "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/resources/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/Framework/MultiMedia/Video/Renderer/FilterGraph/Vulkan/Shaders"
            "$<TARGET_FILE_DIR:${target}>/resources/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Framework/MultiMedia/Video/Renderer/FilterGraph/Vulkan/Shaders/shader_manifest.json"
            "$<TARGET_FILE_DIR:${target}>/resources/shaders/shader_manifest.json"
        VERBATIM
    )
endfunction()

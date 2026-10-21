function(heisenberg_collect_runtime_dlls debug_output release_output)
    set(runtime_directories
        "${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows"
        "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows"
        "${CMAKE_SOURCE_DIR}/Dependence/LibPlacebo/windows"
        "${CMAKE_SOURCE_DIR}/Dependence/MediaInfo"
        "${CMAKE_SOURCE_DIR}/Dependence/Vulkan/windows"
        "${CMAKE_SOURCE_DIR}/Dependence/Lcms2"
        "${CMAKE_SOURCE_DIR}/Dependence/Shaderc/windows"
        "${CMAKE_SOURCE_DIR}/Dependence/OpenColorIO/windows"
    )
    set(debug_files)
    set(release_files)
    foreach(directory IN LISTS runtime_directories)
        file(GLOB_RECURSE release_dlls CONFIGURE_DEPENDS LIST_DIRECTORIES false
            "${directory}/bin/*.dll")
        if(NOT release_dlls)
            message(FATAL_ERROR "No runtime DLLs found in ${directory}/bin. Fetch the Dependence binary files with Git LFS.")
        endif()
        if(IS_DIRECTORY "${directory}/debug/bin")
            file(GLOB_RECURSE debug_dlls CONFIGURE_DEPENDS LIST_DIRECTORIES false
                "${directory}/debug/bin/*.dll")
            if(NOT debug_dlls)
                message(FATAL_ERROR "No Debug runtime DLLs found in ${directory}/debug/bin. Fetch the Dependence binary files with Git LFS.")
            endif()
        else()
            set(debug_dlls ${release_dlls})
        endif()
        list(APPEND debug_files ${debug_dlls})
        list(APPEND release_files ${release_dlls})
    endforeach()
    set(${debug_output} "${debug_files}" PARENT_SCOPE)
    set(${release_output} "${release_files}" PARENT_SCOPE)
endfunction()

function(heisenberg_deploy_runtime target)
    if(NOT WIN32)
        return()
    endif()
    heisenberg_collect_runtime_dlls(debug_runtime_files release_runtime_files)
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
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${debug_runtime_files},${release_runtime_files}>"
            "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/resources/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/Framework/MultiMedia/Video/Renderer/FilterGraph/Vulkan/Shaders"
            "$<TARGET_FILE_DIR:${target}>/resources/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Framework/MultiMedia/Video/Renderer/FilterGraph/Vulkan/Shaders/shader_manifest.json"
            "$<TARGET_FILE_DIR:${target}>/resources/shaders/shader_manifest.json"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endfunction()

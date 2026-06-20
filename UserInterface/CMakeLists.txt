add_executable(Heisenberg main.cpp)
target_link_libraries(Heisenberg PRIVATE HeisenbergUtiles spdlog ffmpeg libplacebo)

if(MSVC)
    target_compile_options(Heisenberg PRIVATE /utf-8)
endif()

if(WIN32)
    add_custom_command(TARGET Heisenberg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/debug/bin/spdlogd.dll,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/bin/spdlog.dll>"
            "$<TARGET_FILE_DIR:Heisenberg>"
    )

    # Copy FFmpeg DLLs to output directory
    add_custom_command(TARGET Heisenberg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avcodec-62.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avdevice-62.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avfilter-11.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avformat-62.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avutil-60.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/swresample-6.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/swscale-9.dll"
            "$<TARGET_FILE_DIR:Heisenberg>"
    )

    # Copy libplacebo DLL to output directory
    add_custom_command(TARGET Heisenberg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Dependence/LibPlacebo/windows/bin/libplacebo-365.dll"
            "$<TARGET_FILE_DIR:Heisenberg>"
    )

    # Copy LCMS2 DLL (libplacebo runtime dependency) to output directory
    add_custom_command(TARGET Heisenberg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/Lcms2/debug/bin/lcms2-2.dll,${CMAKE_SOURCE_DIR}/Dependence/Lcms2/bin/lcms2-2.dll>"
            "$<TARGET_FILE_DIR:Heisenberg>"
    )
endif()
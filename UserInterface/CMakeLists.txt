file(GLOB_RECURSE UI_SOURCES *.cpp *.hpp)

add_executable(MainApp
    ${UI_SOURCES}
)

set_target_properties(MainApp PROPERTIES
    AUTOMOC ON
)

target_include_directories(MainApp PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/Backend
    ${CMAKE_CURRENT_LIST_DIR}/MainWidget
    ${CMAKE_SOURCE_DIR}/Previewer
)

target_link_libraries(MainApp PRIVATE
    HeisenbergPreviewer HeisenbergUtiles
    spdlog ffmpeg libplacebo vulkan volk
    Qt6::Core Qt6::Gui Qt6::Widgets
)

if(MSVC)
    target_compile_options(MainApp PRIVATE /utf-8)
endif()

target_compile_definitions(MainApp PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(WIN32)
    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/debug/bin/spdlogd.dll,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/bin/spdlog.dll>"
            "$<TARGET_FILE_DIR:MainApp>"
    )

    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avcodec-62.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avdevice-62.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avfilter-11.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avformat-62.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avutil-60.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/swresample-6.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/swscale-9.dll"
            "$<TARGET_FILE_DIR:MainApp>"
    )

    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Dependence/LibPlacebo/windows/bin/libplacebo-365.dll"
            "$<TARGET_FILE_DIR:MainApp>"
    )

    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/Lcms2/debug/bin/lcms2-2.dll,${CMAKE_SOURCE_DIR}/Dependence/Lcms2/bin/lcms2-2.dll>"
            "$<TARGET_FILE_DIR:MainApp>"
    )

    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/Vulkan/windows/debug/bin/vulkan-1.dll,${CMAKE_SOURCE_DIR}/Dependence/Vulkan/windows/bin/vulkan-1.dll>"
            "$<TARGET_FILE_DIR:MainApp>"
    )

    # windeployqt for QWidget (不需要 --qmldir)
    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND "${QT_PATH}/bin/windeployqt.exe"
            "$<TARGET_FILE:MainApp>"
            --no-translations
        COMMENT "Deploying Qt6 DLLs..."
    )
endif()

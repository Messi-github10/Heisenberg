add_executable(MainApp
    main.cpp
    qml.qrc
    Backend/MediaPoolModel.hpp        Backend/MediaPoolModel.cpp
    Backend/PlayerController.hpp      Backend/PlayerController.cpp
    Backend/MediaInfoProvider.hpp      Backend/MediaInfoProvider.cpp
    Backend/ColorGradeModel.hpp        Backend/ColorGradeModel.cpp
    Backend/ScopeDataProvider.hpp      Backend/ScopeDataProvider.cpp
)

set_target_properties(MainApp PROPERTIES
    AUTOMOC ON
    AUTORCC ON
)

target_include_directories(MainApp PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/Backend
)

target_link_libraries(MainApp PRIVATE
    HeisenbergPreviewer HeisenbergUtiles
    spdlog ffmpeg libplacebo vulkan volk
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Quick Qt6::QuickControls2 Qt6::QuickLayouts Qt6::QuickDialogs2
)

if(MSVC)
    target_compile_options(MainApp PRIVATE /utf-8)
endif()

if(WIN32)
    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/debug/bin/spdlogd.dll,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/bin/spdlog.dll>"
            "$<TARGET_FILE_DIR:MainApp>"
    )

    # Copy FFmpeg DLLs to output directory
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

    # Copy libplacebo DLL to output directory
    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Dependence/LibPlacebo/windows/bin/libplacebo-365.dll"
            "$<TARGET_FILE_DIR:MainApp>"
    )

    # Copy LCMS2 DLL (libplacebo runtime dependency) to output directory
    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/Lcms2/debug/bin/lcms2-2.dll,${CMAKE_SOURCE_DIR}/Dependence/Lcms2/bin/lcms2-2.dll>"
            "$<TARGET_FILE_DIR:MainApp>"
    )

    # Copy Vulkan DLL to output directory
    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/Vulkan/windows/debug/bin/vulkan-1.dll,${CMAKE_SOURCE_DIR}/Dependence/Vulkan/windows/bin/vulkan-1.dll>"
            "$<TARGET_FILE_DIR:MainApp>"
    )

    # 使用 windeployqt 自动部署 Qt6 DLL 与平台插件到构建输出目录
    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND "${QT_PATH}/bin/windeployqt.exe"
            "$<TARGET_FILE:MainApp>"
            --no-translations
            --qmldir "${CMAKE_CURRENT_LIST_DIR}/Qml"
        COMMENT "Deploying Qt6 DLLs..."
    )
endif()
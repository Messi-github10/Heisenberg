set(QT_PATH "D:/Environment/Qt/6.8.3/msvc2022_64" CACHE PATH "Qt 6 installation root")
list(APPEND CMAKE_PREFIX_PATH "${QT_PATH}")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)

file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS *.cpp *.hpp)

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
    ${CMAKE_SOURCE_DIR}/Framework
)

target_link_libraries(MainApp PRIVATE
    HeisenbergFramework HeisenbergUtiles
    spdlog ffmpeg libplacebo vulkan volk
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent
)

if(MSVC)
    target_compile_options(MainApp PRIVATE /utf-8)
endif()

target_compile_definitions(MainApp PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(WIN32)
    heisenberg_deploy_runtime(MainApp)
    add_custom_command(TARGET MainApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/ffmpeg.exe"
            "$<IF:$<CONFIG:Debug>,${QT_PATH}/bin/Qt6Concurrentd.dll,${QT_PATH}/bin/Qt6Concurrent.dll>"
            "$<TARGET_FILE_DIR:MainApp>"
        COMMAND "${QT_PATH}/bin/windeployqt.exe"
            "$<TARGET_FILE:MainApp>" --no-translations
        COMMENT "Deploying Qt6 DLLs..."
        VERBATIM
    )
endif()

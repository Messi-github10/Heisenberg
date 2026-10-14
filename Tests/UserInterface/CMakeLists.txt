include(GoogleTest)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)

add_executable(HeisenbergEditorUiTests
    media_pool_test.cpp
    timeline_interaction_test.cpp
    ${CMAKE_SOURCE_DIR}/UserInterface/Backend/EditorSession.cpp
    ${CMAKE_SOURCE_DIR}/UserInterface/Backend/EditorSession.hpp
    ${CMAKE_SOURCE_DIR}/UserInterface/Backend/PlayerController.cpp
    ${CMAKE_SOURCE_DIR}/UserInterface/Backend/PlayerController.hpp
    ${CMAKE_SOURCE_DIR}/UserInterface/Backend/MediaInfoProbe.cpp
    ${CMAKE_SOURCE_DIR}/UserInterface/Backend/MediaInfoProbe.hpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/MediaInfoPanel.cpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/MediaInfoPanel.hpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/RoundedPanel.cpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/RoundedPanel.hpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/MediaPoolWidget.cpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/MediaPoolWidget.hpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/TimelineWidget.cpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/TimelineWidget.hpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/MainWindow.cpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/MainWindow.hpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/VideoWidget.cpp
    ${CMAKE_SOURCE_DIR}/UserInterface/MainWidget/VideoWidget.hpp
)
set_target_properties(HeisenbergEditorUiTests PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/UserInterface"
)
add_dependencies(HeisenbergEditorUiTests MainApp HeisenbergFrameworkTestMedia)
target_include_directories(HeisenbergEditorUiTests PRIVATE ${CMAKE_SOURCE_DIR}/UserInterface)
target_link_libraries(HeisenbergEditorUiTests PRIVATE
    HeisenbergFramework GTest::gtest Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent)
target_compile_definitions(HeisenbergEditorUiTests PRIVATE
    HEISENBERG_TEST_MEDIA="${CMAKE_BINARY_DIR}/Tests/Framework/media/playlist_clip_a.mp4"
    HEISENBERG_TEST_MP3="${CMAKE_BINARY_DIR}/Tests/Framework/media/audio_44100_mono.mp3"
    HEISENBERG_TEST_WAV="${CMAKE_BINARY_DIR}/Tests/Framework/media/audio_44100_mono.wav"
    HEISENBERG_TEST_LONG_AUDIO="${CMAKE_BINARY_DIR}/Tests/Framework/media/audio_long.wav"
    HEISENBERG_TEST_COVER_MP3="${CMAKE_BINARY_DIR}/Tests/Framework/media/audio_cover.mp3")
if(MSVC)
    target_compile_options(HeisenbergEditorUiTests PRIVATE /utf-8)
endif()
if(WIN32)
    add_custom_command(TARGET HeisenbergEditorUiTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:HeisenbergEditorUiTests>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${QT_PATH}/plugins/platforms/qoffscreend.dll,${QT_PATH}/plugins/platforms/qoffscreen.dll>"
            "$<TARGET_FILE_DIR:HeisenbergEditorUiTests>/platforms"
    )
endif()
gtest_discover_tests(HeisenbergEditorUiTests)

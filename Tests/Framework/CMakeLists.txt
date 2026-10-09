include(GoogleTest)

set(FFMPEG_EXE "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/ffmpeg.exe")
set(FRAMEWORK_TEST_MEDIA_DIR "${CMAKE_CURRENT_BINARY_DIR}/media")

set(FRAMEWORK_TEST_MEDIA
    "${FRAMEWORK_TEST_MEDIA_DIR}/producer.mp4")
set(FRAMEWORK_TEST_CLIP_A
    "${FRAMEWORK_TEST_MEDIA_DIR}/playlist_clip_a.mp4")
set(FRAMEWORK_TEST_CLIP_B
    "${FRAMEWORK_TEST_MEDIA_DIR}/playlist_clip_b.mp4")

add_custom_command(
    OUTPUT "${FRAMEWORK_TEST_MEDIA}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FRAMEWORK_TEST_MEDIA_DIR}"
    COMMAND "${FFMPEG_EXE}"
            -y -hide_banner -loglevel error
            -f lavfi -i testsrc=size=320x240:rate=24:duration=1
            -f lavfi -i sine=frequency=440:sample_rate=48000:duration=1
            -pix_fmt yuv420p -c:v libx264 -preset ultrafast -c:a aac
            "${FRAMEWORK_TEST_MEDIA}"
    COMMENT "Generating Framework producer test media"
    VERBATIM
)
add_custom_command(
    OUTPUT "${FRAMEWORK_TEST_CLIP_A}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FRAMEWORK_TEST_MEDIA_DIR}"
    COMMAND "${FFMPEG_EXE}"
            -y -hide_banner -loglevel error
            -f lavfi -i testsrc=size=320x240:rate=24:duration=1
            -f lavfi -i sine=frequency=440:sample_rate=48000:duration=1
            -pix_fmt yuv420p -c:v libx264 -preset ultrafast -c:a aac
            "${FRAMEWORK_TEST_CLIP_A}"
    COMMENT "Generating Framework playlist clip A"
    VERBATIM
)
add_custom_command(
    OUTPUT "${FRAMEWORK_TEST_CLIP_B}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FRAMEWORK_TEST_MEDIA_DIR}"
    COMMAND "${FFMPEG_EXE}"
            -y -hide_banner -loglevel error
            -f lavfi -i testsrc=size=160x120:rate=24:duration=1
            -f lavfi -i sine=frequency=880:sample_rate=48000:duration=1
            -pix_fmt yuv420p -c:v libx264 -preset ultrafast -c:a aac
            "${FRAMEWORK_TEST_CLIP_B}"
    COMMENT "Generating Framework playlist clip B"
    VERBATIM
)

add_custom_target(HeisenbergFrameworkTestMedia
    DEPENDS
        "${FRAMEWORK_TEST_MEDIA}"
        "${FRAMEWORK_TEST_CLIP_A}"
        "${FRAMEWORK_TEST_CLIP_B}")

add_executable(HeisenbergFrameworkTests
    profile_test.cpp
    producer_test.cpp
    blank_test.cpp
    timeline_test.cpp
    editor_test.cpp
)
add_dependencies(HeisenbergFrameworkTests HeisenbergFrameworkTestMedia)
target_link_libraries(HeisenbergFrameworkTests PRIVATE
    HeisenbergFramework
    GTest::gtest_main
)
target_compile_definitions(HeisenbergFrameworkTests PRIVATE
    HEISENBERG_TEST_MEDIA="${FRAMEWORK_TEST_MEDIA}"
    HEISENBERG_PLAYLIST_CLIP_A="${FRAMEWORK_TEST_CLIP_A}"
    HEISENBERG_PLAYLIST_CLIP_B="${FRAMEWORK_TEST_CLIP_B}"
)

if(MSVC)
    target_compile_options(HeisenbergFrameworkTests PRIVATE /utf-8)
endif()

if(WIN32)
    add_custom_command(TARGET HeisenbergFrameworkTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/debug/bin/spdlogd.dll,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/bin/spdlog.dll>"
            "$<TARGET_FILE_DIR:HeisenbergFrameworkTests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avcodec-62.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avdevice-62.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avfilter-11.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avformat-62.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/avutil-60.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/swresample-6.dll"
            "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/swscale-9.dll"
            "$<TARGET_FILE_DIR:HeisenbergFrameworkTests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Dependence/LibPlacebo/windows/bin/libplacebo-365.dll"
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/Vulkan/windows/debug/bin/vulkan-1.dll,${CMAKE_SOURCE_DIR}/Dependence/Vulkan/windows/bin/vulkan-1.dll>"
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/Lcms2/debug/bin/lcms2-2.dll,${CMAKE_SOURCE_DIR}/Dependence/Lcms2/bin/lcms2-2.dll>"
            "$<TARGET_FILE_DIR:HeisenbergFrameworkTests>"
    )
endif()

gtest_discover_tests(HeisenbergFrameworkTests)

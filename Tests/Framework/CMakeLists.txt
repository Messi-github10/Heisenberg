include(GoogleTest)

set(FFMPEG_EXE "${CMAKE_SOURCE_DIR}/Dependence/FFmpeg/windows/bin/ffmpeg.exe")
set(FRAMEWORK_TEST_MEDIA_DIR "${CMAKE_CURRENT_BINARY_DIR}/media")

set(FRAMEWORK_TEST_MEDIA
    "${FRAMEWORK_TEST_MEDIA_DIR}/producer.mp4")
set(FRAMEWORK_TEST_CLIP_A
    "${FRAMEWORK_TEST_MEDIA_DIR}/playlist_clip_a.mp4")
set(FRAMEWORK_TEST_CLIP_B
    "${FRAMEWORK_TEST_MEDIA_DIR}/playlist_clip_b.mp4")
set(FRAMEWORK_TEST_SDR_GRAY
    "${FRAMEWORK_TEST_MEDIA_DIR}/sdr_gray_patches.mp4")
add_custom_command(
    OUTPUT "${FRAMEWORK_TEST_SDR_GRAY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FRAMEWORK_TEST_MEDIA_DIR}"
    COMMAND "${FFMPEG_EXE}" -y -hide_banner -loglevel error
            -f lavfi -i "nullsrc=s=160x120:r=24:d=0.25,format=yuv420p,geq=lum='if(lt(X,20),16,if(lt(X,40),24,if(lt(X,60),40,if(lt(X,80),64,if(lt(X,100),96,if(lt(X,120),128,if(lt(X,140),192,235)))))))':cb=128:cr=128"
            -c:v libx264 -crf 0 -preset ultrafast
            -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv
            -bsf:v h264_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1
            "${FRAMEWORK_TEST_SDR_GRAY}"
    DEPENDS "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "Generating tagged SDR gray patches for appearance regression"
    VERBATIM
)

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

set(FRAMEWORK_TEST_MP3 "${FRAMEWORK_TEST_MEDIA_DIR}/audio_44100_mono.mp3")
set(FRAMEWORK_TEST_WAV "${FRAMEWORK_TEST_MEDIA_DIR}/audio_44100_mono.wav")
set(FRAMEWORK_TEST_FLOAT_WAV "${FRAMEWORK_TEST_MEDIA_DIR}/audio_float.wav")
set(FRAMEWORK_TEST_AAC "${FRAMEWORK_TEST_MEDIA_DIR}/audio_aac.m4a")
set(FRAMEWORK_TEST_FLAC "${FRAMEWORK_TEST_MEDIA_DIR}/audio_flac.flac")
set(FRAMEWORK_TEST_OGG "${FRAMEWORK_TEST_MEDIA_DIR}/audio_vorbis.ogg")
set(FRAMEWORK_TEST_LONG_AUDIO "${FRAMEWORK_TEST_MEDIA_DIR}/audio_long.wav")
set(FRAMEWORK_TEST_COVER "${FRAMEWORK_TEST_MEDIA_DIR}/audio_cover.jpg")
set(FRAMEWORK_TEST_COVER_MP3 "${FRAMEWORK_TEST_MEDIA_DIR}/audio_cover.mp3")

function(add_audio_fixture output codec sample_rate channels duration)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${FRAMEWORK_TEST_MEDIA_DIR}"
        COMMAND "${FFMPEG_EXE}" -y -hide_banner -loglevel error
                -f lavfi -i "sine=frequency=880:sample_rate=${sample_rate}:duration=${duration}"
                -ac "${channels}" -c:a "${codec}" "${output}"
        VERBATIM
    )
endfunction()
add_audio_fixture("${FRAMEWORK_TEST_MP3}" libmp3lame 44100 1 1.3)
add_audio_fixture("${FRAMEWORK_TEST_WAV}" pcm_s16le 44100 1 1.3)
add_audio_fixture("${FRAMEWORK_TEST_FLOAT_WAV}" pcm_f32le 48000 2 1.3)
add_audio_fixture("${FRAMEWORK_TEST_AAC}" aac 48000 2 1.3)
add_audio_fixture("${FRAMEWORK_TEST_FLAC}" flac 44100 1 1.3)
add_audio_fixture("${FRAMEWORK_TEST_OGG}" libvorbis 44100 1 1.3)
add_audio_fixture("${FRAMEWORK_TEST_LONG_AUDIO}" pcm_s16le 48000 2 3)
add_custom_command(
    OUTPUT "${FRAMEWORK_TEST_COVER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FRAMEWORK_TEST_MEDIA_DIR}"
    COMMAND "${FFMPEG_EXE}" -y -hide_banner -loglevel error
            -f lavfi -i color=c=blue:s=64x64 -frames:v 1 -update 1 "${FRAMEWORK_TEST_COVER}"
    VERBATIM
)
add_custom_command(
    OUTPUT "${FRAMEWORK_TEST_COVER_MP3}"
    COMMAND "${FFMPEG_EXE}" -y -hide_banner -loglevel error
            -i "${FRAMEWORK_TEST_MP3}" -i "${FRAMEWORK_TEST_COVER}"
            -map 0:a -map 1:v -c copy -id3v2_version 3
            -disposition:v attached_pic "${FRAMEWORK_TEST_COVER_MP3}"
    DEPENDS "${FRAMEWORK_TEST_MP3}" "${FRAMEWORK_TEST_COVER}"
    VERBATIM
)

add_custom_target(HeisenbergFrameworkTestMedia
    DEPENDS
        "${FRAMEWORK_TEST_MP3}"
        "${FRAMEWORK_TEST_WAV}"
        "${FRAMEWORK_TEST_FLOAT_WAV}"
        "${FRAMEWORK_TEST_AAC}"
        "${FRAMEWORK_TEST_FLAC}"
        "${FRAMEWORK_TEST_OGG}"
        "${FRAMEWORK_TEST_LONG_AUDIO}"
        "${FRAMEWORK_TEST_COVER_MP3}"
        "${FRAMEWORK_TEST_MEDIA}"
        "${FRAMEWORK_TEST_CLIP_A}"
        "${FRAMEWORK_TEST_CLIP_B}"
        "${FRAMEWORK_TEST_SDR_GRAY}")

add_executable(HeisenbergFrameworkTests
    timeline_settings_test.cpp
    video_engine_source_test.cpp
    frame_renderer_test.cpp
    timeline_test.cpp
    editor_test.cpp
    render_thread_test.cpp
    media_math_test.cpp
    composition_plan_test.cpp
    audio_mixer_test.cpp
    video_engine_transport_test.cpp
    native_preview_test.cpp
    independent_audio_editor_test.cpp
    independent_audio_engine_test.cpp
    export_engine_test.cpp
    grade_test.cpp
    encoding_test.cpp
    composition_executor_test.cpp
    color_pipeline_test.cpp
    grade_edit_continuity_test.cpp
    grade_renderer_test.cpp
    composition_renderer_interop_test.cpp
    color_settings_test.cpp
    video_presenter_test.cpp
)
add_dependencies(HeisenbergFrameworkTests HeisenbergFrameworkTestMedia)
target_link_libraries(HeisenbergFrameworkTests PRIVATE
    HeisenbergFramework
    GTest::gtest_main
)
target_compile_definitions(HeisenbergFrameworkTests PRIVATE
    HEISENBERG_TEST_MEDIA="${FRAMEWORK_TEST_MEDIA}"
    HEISENBERG_TEST_MP3="${FRAMEWORK_TEST_MP3}"
    HEISENBERG_TEST_WAV="${FRAMEWORK_TEST_WAV}"
    HEISENBERG_TEST_FLOAT_WAV="${FRAMEWORK_TEST_FLOAT_WAV}"
    HEISENBERG_TEST_AAC="${FRAMEWORK_TEST_AAC}"
    HEISENBERG_TEST_FLAC="${FRAMEWORK_TEST_FLAC}"
    HEISENBERG_TEST_OGG="${FRAMEWORK_TEST_OGG}"
    HEISENBERG_TEST_LONG_AUDIO="${FRAMEWORK_TEST_LONG_AUDIO}"
    HEISENBERG_TEST_COVER_MP3="${FRAMEWORK_TEST_COVER_MP3}"
    HEISENBERG_PLAYLIST_CLIP_A="${FRAMEWORK_TEST_CLIP_A}"
    HEISENBERG_PLAYLIST_CLIP_B="${FRAMEWORK_TEST_CLIP_B}"
    HEISENBERG_TEST_SDR_GRAY="${FRAMEWORK_TEST_SDR_GRAY}"
    HEISENBERG_TEST_FILTER="${CMAKE_SOURCE_DIR}/Framework/MultiMedia/Video/Renderer/FilterGraph/Vulkan/Test/exposure.json"
)

if(MSVC)
    target_compile_options(HeisenbergFrameworkTests PRIVATE /utf-8)
endif()

heisenberg_deploy_runtime(HeisenbergFrameworkTests)

gtest_discover_tests(HeisenbergFrameworkTests)

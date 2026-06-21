add_library(HeisenbergPreviewer STATIC
    Common/Packet.cpp
    Demuxer/Demuxer.cpp
    Demuxer/DemuxerFactory.cpp
)

target_include_directories(HeisenbergPreviewer PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(HeisenbergPreviewer PUBLIC
    ffmpeg
)

if(MSVC)
    target_compile_options(HeisenbergPreviewer PRIVATE /utf-8)
endif()

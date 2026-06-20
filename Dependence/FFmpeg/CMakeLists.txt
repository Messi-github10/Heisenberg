# FFmpeg — pre-built Windows binaries (LGPL licensed)
add_library(ffmpeg INTERFACE)

target_include_directories(ffmpeg INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/windows/include"
)

target_link_libraries(ffmpeg INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/windows/lib/avcodec.lib
    ${CMAKE_CURRENT_LIST_DIR}/windows/lib/avdevice.lib
    ${CMAKE_CURRENT_LIST_DIR}/windows/lib/avfilter.lib
    ${CMAKE_CURRENT_LIST_DIR}/windows/lib/avformat.lib
    ${CMAKE_CURRENT_LIST_DIR}/windows/lib/avutil.lib
    ${CMAKE_CURRENT_LIST_DIR}/windows/lib/swresample.lib
    ${CMAKE_CURRENT_LIST_DIR}/windows/lib/swscale.lib
)

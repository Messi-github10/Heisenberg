add_library(miniaudio STATIC
    "${CMAKE_CURRENT_LIST_DIR}/miniaudio_impl.c"
)

target_include_directories(miniaudio PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
)

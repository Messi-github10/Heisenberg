# libplacebo — pre-built Windows binaries (LGPL licensed)
add_library(libplacebo INTERFACE)

target_include_directories(libplacebo INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/windows/include"
)

target_link_libraries(libplacebo INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/windows/lib/libplacebo.lib
    lcms2  # runtime dependency
)

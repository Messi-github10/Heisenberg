# LCMS2 (Little CMS) — pre-built Windows binaries (MIT licensed)
# Runtime dependency of libplacebo for color management
add_library(lcms2 INTERFACE)

target_include_directories(lcms2 INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/include"
)

target_link_libraries(lcms2 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/lib/lcms2.lib
)

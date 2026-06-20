# spdlog — header-only fast C++ logging library (pre-built Windows binaries)
add_library(spdlog INTERFACE)

target_include_directories(spdlog INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/windows/include"
)

target_link_libraries(spdlog INTERFACE
    "$<IF:$<CONFIG:Debug>,${CMAKE_CURRENT_LIST_DIR}/windows/debug/lib/spdlogd.lib,${CMAKE_CURRENT_LIST_DIR}/windows/lib/spdlog.lib>"
)
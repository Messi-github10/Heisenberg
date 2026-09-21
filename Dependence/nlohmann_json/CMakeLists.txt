# nlohmann/json — header-only JSON for Modern C++
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/include"
)

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS *.cpp *.hpp)

add_library(HeisenbergCore STATIC
    ${CORE_SOURCES}
)

target_include_directories(HeisenbergCore PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(HeisenbergCore PUBLIC
    nlohmann_json
)

if(MSVC)
    target_compile_options(HeisenbergCore PRIVATE /utf-8)
endif()

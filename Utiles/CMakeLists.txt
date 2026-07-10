file(GLOB UTILES_SOURCES *.cpp *.hpp)

add_library(HeisenbergUtiles STATIC
    ${UTILES_SOURCES}
)

target_include_directories(HeisenbergUtiles PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(HeisenbergUtiles PUBLIC spdlog)

target_compile_definitions(HeisenbergUtiles PUBLIC
    HEISENBERG_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

if(MSVC)
    target_compile_options(HeisenbergUtiles PRIVATE /utf-8)
endif()

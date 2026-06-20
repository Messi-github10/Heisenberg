add_executable(Heisenberg main.cpp)
target_link_libraries(Heisenberg PRIVATE HeisenbergUtiles spdlog)

# spdlog/fmt bundled requires /utf-8 on MSVC
if(MSVC)
    target_compile_options(Heisenberg PRIVATE /utf-8)
endif()

# Copy spdlog DLL to output directory on Windows (debug/release)
if(WIN32)
    add_custom_command(TARGET Heisenberg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/debug/bin/spdlogd.dll,${CMAKE_SOURCE_DIR}/Dependence/SpdLog/windows/bin/spdlog.dll>"
            "$<TARGET_FILE_DIR:Heisenberg>"
    )
endif()
cmake_minimum_required(VERSION 3.21)
project(Heisenberg)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
enable_testing()

option(HEISENBERG_BUILD_UI "Build the Qt desktop application and its tests" ON)
include(CMake/DeployRuntime.cmake)

add_subdirectory(Dependence)
add_subdirectory(Utiles)
add_subdirectory(Framework)
if(HEISENBERG_BUILD_UI)
    add_subdirectory(UserInterface)
endif()
if(BUILD_TESTING)
    add_subdirectory(Tests)
endif()

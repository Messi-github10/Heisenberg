cmake_minimum_required(VERSION 3.21)
project(Heisenberg)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
enable_testing()

add_subdirectory(Dependence)
add_subdirectory(Utiles)
add_subdirectory(Core)
add_subdirectory(Previewer)
add_subdirectory(UserInterface)
add_subdirectory(Tests)

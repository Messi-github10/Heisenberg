cmake_minimum_required(VERSION 3.21)
project(Heisenberg)

set(CMAKE_CXX_STANDARD 20)

set(QT_PATH "D:/Environment/Qt/6.8.3/msvc2022_64" CACHE PATH "Qt 6 installation root")
list(APPEND CMAKE_PREFIX_PATH "${QT_PATH}")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_subdirectory(Dependence)
add_subdirectory(Previewer)
add_subdirectory(Utiles)
add_subdirectory(UserInterface)


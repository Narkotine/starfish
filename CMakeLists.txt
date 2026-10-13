cmake_minimum_required(VERSION 3.24)

project(StarFish
    VERSION 0.1.0
    DESCRIPTION "Open-source Fences 6 equivalent for Windows"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Test)
qt_standard_project_setup()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

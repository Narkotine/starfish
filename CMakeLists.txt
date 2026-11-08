cmake_minimum_required(VERSION 3.24)

project(StarFish
    VERSION 0.1.2
    DESCRIPTION "Open-source Fences 6 equivalent for Windows"
    LANGUAGES CXX
)

# The application manifest is load bearing, not cosmetic: without it Windows
# applies pre-Windows-8 compatibility behaviour, WS_EX_LAYERED cannot be added to
# a child window, and the hosted zones are never composited by DWM. See
# src/starfish.manifest and ADR-4. Enabled explicitly because MinGW has no
# default manifest step, unlike MSVC.
enable_language(RC)

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

# Per-user installer (Inno Setup, see installer/starfish.iss). Not part of ALL.
if(WIN32)
    add_custom_target(installer
        COMMAND pwsh -NoProfile -File "${CMAKE_SOURCE_DIR}/tools/build_installer.ps1" -BuildDir "${CMAKE_BINARY_DIR}"
        DEPENDS starfish
        COMMENT "Building the StarFish setup"
        USES_TERMINAL)
endif()

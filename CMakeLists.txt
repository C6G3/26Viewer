cmake_minimum_required(VERSION 3.25)

project(26Viewer
    VERSION 0.1.0
    DESCRIPTION "Modular game archive viewer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)

# ---- Options ---------------------------------------------------------------
option(VIEWER_BUILD_APP   "Build the Qt desktop application" ON)
option(VIEWER_BUILD_CLI   "Build the command-line tool"      ON)
option(VIEWER_BUILD_TESTS "Build unit tests"                 ON)

# Which game modules get compiled in. Each name maps to src/games/<name>.
# Example: cmake --preset release -DVIEWER_GAMES="blur"
set(VIEWER_GAMES "blur" CACHE STRING "Semicolon-separated list of game modules to build")

# ---- Dependencies ----------------------------------------------------------
find_package(ZLIB REQUIRED)

# ---- Targets ---------------------------------------------------------------
add_subdirectory(src/core)
include(Games)          # adds src/games/<name> and the generated registry
add_subdirectory(src/games)

if(VIEWER_BUILD_CLI)
    add_subdirectory(src/cli)
endif()

if(VIEWER_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(VIEWER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

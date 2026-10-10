# Shared compile settings applied through the `viewer_options` interface target.

add_library(viewer_options INTERFACE)
add_library(viewer::options ALIAS viewer_options)

if(MSVC)
    target_compile_options(viewer_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(viewer_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
        -Wcast-align -Woverloaded-virtual -Wnull-dereference
        -Wimplicit-fallthrough)
endif()

if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # libstdc++ ships the Windows console parts of std::print in a separate library.
    target_link_libraries(viewer_options INTERFACE stdc++exp)
endif()

if(WIN32)
    # Target Windows 10+ and keep <windows.h> lean.
    target_compile_definitions(viewer_options INTERFACE
        _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

# Everything a build produces lands in build/<preset>/bin so the app,
# CLI and tests can find each other's DLLs without copying.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

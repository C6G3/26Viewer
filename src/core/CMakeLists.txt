# viewer_core — game-agnostic foundation shared by every game module, the
# CLI and the GUI. It must never depend on Qt so that it stays reusable
# (command-line tools, tests, future front-ends).

add_library(viewer_core STATIC
    include/viewer/core/Archive.hpp
    include/viewer/core/Bytes.hpp
    include/viewer/core/Compression.hpp
    include/viewer/core/ContentRegistry.hpp
    include/viewer/core/Error.hpp
    include/viewer/core/FileTypes.hpp
    include/viewer/core/Game.hpp
    include/viewer/core/GameRegistry.hpp
    include/viewer/core/Hash.hpp
    include/viewer/core/Image.hpp
    include/viewer/core/RandomAccessFile.hpp
    include/viewer/core/Vfs.hpp
    src/Bytes.cpp
    src/Compression.cpp
    src/ContentRegistry.cpp
    src/FileTypes.cpp
    src/GameRegistry.cpp
    src/Hash.cpp
    src/Image.cpp
    src/RandomAccessFile.cpp
    src/Vfs.cpp
)
add_library(viewer::core ALIAS viewer_core)

target_include_directories(viewer_core PUBLIC include)
target_link_libraries(viewer_core
    PUBLIC  viewer::options
    PRIVATE ZLIB::ZLIB)

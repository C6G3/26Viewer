# Blur (Bizarre Creations, 2010)

add_library(viewer_game_blur STATIC
    AudioEntity.cpp
    AudioEntity.hpp
    BafBank.cpp
    BafBank.hpp
    BlurGame.cpp
    ChunkFile.cpp
    ChunkFile.hpp
    ModelDecoder.cpp
    ModelDecoder.hpp
    PakArchive.cpp
    PakArchive.hpp
)
target_include_directories(viewer_game_blur PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(viewer_game_blur PUBLIC viewer::core)

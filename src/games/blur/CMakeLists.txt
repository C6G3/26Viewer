# Blur (Bizarre Creations, 2010)

add_library(viewer_game_blur STATIC
    Animation.cpp
    Animation.hpp
    AudioEntity.cpp
    AudioEntity.hpp
    BafBank.cpp
    BafBank.hpp
    BlurGame.cpp
    ChunkFile.cpp
    ChunkFile.hpp
    FlaskTable.cpp
    FlaskTable.hpp
    FontFile.cpp
    FontFile.hpp
    Localisation.cpp
    Localisation.hpp
    ModelDecoder.cpp
    ModelDecoder.hpp
    Level.cpp
    Level.hpp
    Physics.cpp
    Physics.hpp
    PakArchive.cpp
    PakArchive.hpp
    UiFontTable.cpp
    UiFontTable.hpp
    UiLayout.cpp
    UiLayout.hpp
    UiPack.cpp
    UiPack.hpp
)
target_include_directories(viewer_game_blur PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(viewer_game_blur PUBLIC viewer::core)

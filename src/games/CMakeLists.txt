# Builds each selected game module, then `viewer_games`, which bundles them
# behind a single registerBuiltinGames() entry point (see cmake/Games.cmake).

foreach(game IN LISTS VIEWER_GAMES)
    add_subdirectory(${game})
endforeach()

add_library(viewer_games STATIC
    GameModules.hpp
    "${VIEWER_GENERATED_GAME_SOURCE}")
add_library(viewer::games ALIAS viewer_games)

target_include_directories(viewer_games PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(viewer_games
    PUBLIC  viewer::core
    PRIVATE viewer::options ${VIEWER_GAME_TARGETS})

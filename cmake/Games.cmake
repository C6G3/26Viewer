# Game module selection.
#
# Every directory in src/games/<name> is a self-contained module that builds a
# static library `viewer_game_<name>` and defines
#     void viewer::games::<name>::registerModule(viewer::GameRegistry&);
#
# VIEWER_GAMES picks which modules are compiled in. This file validates the list
# and generates GameModules.cpp, which calls each module's registerModule().
# Adding a game therefore never requires editing shared code: create the folder,
# then add its name to VIEWER_GAMES.

set(_games_root "${CMAKE_SOURCE_DIR}/src/games")
set(VIEWER_GAME_TARGETS "")
set(_decls "")
set(_calls "")

foreach(game IN LISTS VIEWER_GAMES)
    if(NOT game MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "Invalid game module name '${game}' (use lowercase identifiers)")
    endif()
    if(NOT EXISTS "${_games_root}/${game}/CMakeLists.txt")
        message(FATAL_ERROR "Game module '${game}' not found at ${_games_root}/${game}")
    endif()
    list(APPEND VIEWER_GAME_TARGETS "viewer_game_${game}")
    string(APPEND _decls "namespace viewer::games::${game} { void registerModule(GameRegistry&); }\n")
    string(APPEND _calls "    viewer::games::${game}::registerModule(registry);\n")
endforeach()

list(LENGTH VIEWER_GAMES _game_count)
message(STATUS "26Viewer game modules (${_game_count}): ${VIEWER_GAMES}")

set(VIEWER_GAME_DECLARATIONS "${_decls}")
set(VIEWER_GAME_REGISTRATIONS "${_calls}")
set(VIEWER_GENERATED_GAME_SOURCE "${CMAKE_BINARY_DIR}/generated/GameModules.cpp")
configure_file("${CMAKE_SOURCE_DIR}/cmake/GameModules.cpp.in" "${VIEWER_GENERATED_GAME_SOURCE}" @ONLY)

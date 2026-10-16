# viewer_mcp — the Model Context Protocol server (26viewer-cli mcp): lets AI
# assistants browse, read, search and (when allowed) edit a game's files
# through the same core and game modules as the app. No Qt.

add_library(viewer_mcp STATIC
    McpServer.cpp
    McpServer.hpp
)
add_library(viewer::mcp ALIAS viewer_mcp)

target_include_directories(viewer_mcp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(viewer_mcp
    PUBLIC  viewer::core
    PRIVATE viewer::games viewer::options)

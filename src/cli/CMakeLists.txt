add_executable(viewer_cli main.cpp)
set_target_properties(viewer_cli PROPERTIES OUTPUT_NAME "26viewer-cli")
target_link_libraries(viewer_cli PRIVATE viewer::core viewer::games viewer::mcp viewer::options)
target_compile_definitions(viewer_cli PRIVATE VIEWER_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(viewer_cli PRIVATE shell32)
endif()

install(TARGETS viewer_cli RUNTIME DESTINATION .)

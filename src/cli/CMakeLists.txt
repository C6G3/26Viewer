add_executable(viewer_cli main.cpp)
set_target_properties(viewer_cli PROPERTIES OUTPUT_NAME "26viewer-cli")
target_link_libraries(viewer_cli PRIVATE viewer::core viewer::games viewer::options)
if(WIN32)
    target_link_libraries(viewer_cli PRIVATE shell32)
endif()

install(TARGETS viewer_cli RUNTIME DESTINATION .)

# 26Viewer desktop application (Qt 6 Widgets).

# Svg is linked so deployment picks up the SVG icon engine plugin.
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Concurrent Svg)
qt_standard_project_setup()

qt_add_executable(viewer_app WIN32
    main.cpp
    Extractor.cpp
    Extractor.hpp
    GamePickerDialog.cpp
    GamePickerDialog.hpp
    Icons.cpp
    Icons.hpp
    MainWindow.cpp
    MainWindow.hpp
    QtConvert.hpp
    Session.cpp
    Session.hpp
    Theme.cpp
    Theme.hpp
    VfsModel.cpp
    VfsModel.hpp
    preview/HexView.cpp
    preview/HexView.hpp
    preview/ImageView.cpp
    preview/ImageView.hpp
    preview/PreviewPane.cpp
    preview/PreviewPane.hpp
)
set_target_properties(viewer_app PROPERTIES OUTPUT_NAME "26Viewer")

file(GLOB _type_icons CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/resources/icons/types/*.svg")
qt_add_resources(viewer_app "app_resources"
    PREFIX "/"
    BASE "${PROJECT_SOURCE_DIR}/resources"
    FILES
        "${PROJECT_SOURCE_DIR}/resources/icons/app.svg"
        ${_type_icons})

target_compile_definitions(viewer_app PRIVATE
    VIEWER_VERSION="${PROJECT_VERSION}"
    QT_DISABLE_DEPRECATED_UP_TO=0x060800)
target_link_libraries(viewer_app PRIVATE
    viewer::core viewer::games viewer::options
    Qt6::Widgets Qt6::Concurrent Qt6::Svg)

# ---- Install / deploy ------------------------------------------------------------
# `cmake --install build/<preset>` copies the app into dist/<preset> and makes
# the folder self-contained (see cmake/Deploy.cmake and docs/BUILDING.md).
install(TARGETS viewer_app RUNTIME DESTINATION .)

if(WIN32)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
    find_program(VIEWER_WINDEPLOYQT NAMES windeployqt6 windeployqt HINTS "${_qt_bin}")
    get_filename_component(_cxx_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    install(CODE "
        set(VIEWER_WINDEPLOYQT \"${VIEWER_WINDEPLOYQT}\")
        set(VIEWER_DLL_DIRS \"${_qt_bin};${_cxx_bin}\")
        include(\"${PROJECT_SOURCE_DIR}/cmake/Deploy.cmake\")")
endif()

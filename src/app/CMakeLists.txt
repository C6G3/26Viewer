# 26Viewer desktop application (Qt 6 Widgets).

# Svg is linked so deployment picks up the SVG icon engine plugin.
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Concurrent Svg OpenGLWidgets Multimedia MultimediaWidgets)
qt_standard_project_setup()

qt_add_executable(viewer_app WIN32
    main.cpp
    AssetEditor.cpp
    AssetEditor.hpp
    BackupBanner.cpp
    BackupBanner.hpp
    Extractor.cpp
    Extractor.hpp
    Exporter.cpp
    Exporter.hpp
    FolderView.cpp
    FolderView.hpp
    GamePickerDialog.cpp
    GamePickerDialog.hpp
    Icons.cpp
    Icons.hpp
    ImportExportDialogs.cpp
    ImportExportDialogs.hpp
    MainWindow.cpp
    MainWindow.hpp
    PreviewWindow.cpp
    PreviewWindow.hpp
    QtConvert.hpp
    Session.cpp
    Session.hpp
    TextEditorWindow.cpp
    TextEditorWindow.hpp
    TextureBrowser.cpp
    TextureBrowser.hpp
    Theme.cpp
    Theme.hpp
    VfsModel.cpp
    VfsModel.hpp
    preview/HexView.cpp
    preview/HexView.hpp
    preview/ImageView.cpp
    preview/ImageView.hpp
    preview/ModelView.cpp
    preview/ModelView.hpp
    preview/PreviewPane.cpp
    preview/PreviewPane.hpp
    preview/SoundBankView.cpp
    preview/SoundBankView.hpp
    preview/SyntaxHighlighter.cpp
    preview/SyntaxHighlighter.hpp
    preview/TextView.cpp
    preview/TextView.hpp
    preview/VideoView.cpp
    preview/VideoView.hpp
)
set_target_properties(viewer_app PROPERTIES OUTPUT_NAME "26Viewer")

file(GLOB _type_icons CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/resources/icons/types/*.svg")
qt_add_resources(viewer_app "app_resources"
    PREFIX "/"
    BASE "${PROJECT_SOURCE_DIR}/resources"
    FILES
        "${PROJECT_SOURCE_DIR}/resources/icons/app.svg"
        "${PROJECT_SOURCE_DIR}/resources/icons/edit-mode.svg"
        "${PROJECT_SOURCE_DIR}/resources/icons/textures.svg"
        ${_type_icons})

target_compile_definitions(viewer_app PRIVATE
    VIEWER_VERSION="${PROJECT_VERSION}"
    QT_DISABLE_DEPRECATED_UP_TO=0x060800)
target_link_libraries(viewer_app PRIVATE
    viewer::core viewer::games viewer::options
    Qt6::Widgets Qt6::Concurrent Qt6::Svg Qt6::OpenGLWidgets Qt6::Multimedia Qt6::MultimediaWidgets)

if(WIN32)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
endif()

# ---- Windows .exe icon + version info ----------------------------------------------
# tools/icongen renders resources/icons/app.svg into a multi-size .ico at build
# time, which app.rc embeds together with VERSIONINFO.
if(WIN32)
    enable_language(RC)
    add_executable(viewer_icongen "${PROJECT_SOURCE_DIR}/tools/icongen/main.cpp")
    target_link_libraries(viewer_icongen PRIVATE Qt6::Gui Qt6::Svg)

    set(VIEWER_ICON_FILE "${CMAKE_CURRENT_BINARY_DIR}/app.ico")
    add_custom_command(
        OUTPUT "${VIEWER_ICON_FILE}"
        COMMAND "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_prepend:${_qt_bin}"
                "$<TARGET_FILE:viewer_icongen>" "${PROJECT_SOURCE_DIR}/resources/icons/app.svg" "${VIEWER_ICON_FILE}"
        DEPENDS viewer_icongen "${PROJECT_SOURCE_DIR}/resources/icons/app.svg"
        COMMENT "Rendering app.ico from app.svg"
        VERBATIM)

    configure_file(app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    target_sources(viewer_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc" "${VIEWER_ICON_FILE}")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/app.rc" PROPERTIES OBJECT_DEPENDS "${VIEWER_ICON_FILE}")
endif()

# ---- Install / deploy ------------------------------------------------------------
# `cmake --install build/<preset>` copies the app into dist/<preset> and makes
# the folder self-contained (see cmake/Deploy.cmake and docs/BUILDING.md).
install(TARGETS viewer_app RUNTIME DESTINATION .)

if(WIN32)
    find_program(VIEWER_WINDEPLOYQT NAMES windeployqt6 windeployqt HINTS "${_qt_bin}")
    get_filename_component(_cxx_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    install(CODE "
        set(VIEWER_WINDEPLOYQT \"${VIEWER_WINDEPLOYQT}\")
        set(VIEWER_DLL_DIRS \"${_qt_bin};${_cxx_bin}\")
        include(\"${PROJECT_SOURCE_DIR}/cmake/Deploy.cmake\")")
endif()

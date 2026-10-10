# Install-time deployment (run by `cmake --install`), producing a
# self-contained folder:
#
#   dist/<preset>/26Viewer.exe, 26viewer-cli.exe
#                 Qt6*.dll + every MinGW/MSYS2 DLL they need
#                 platforms/ styles/ imageformats/ iconengines/ ...
#
# Inputs (set by install(CODE) in src/app/CMakeLists.txt):
#   VIEWER_WINDEPLOYQT   path to windeployqt6.exe
#   VIEWER_DLL_DIRS      directories to resolve non-system DLLs from

set(_prefix "${CMAKE_INSTALL_PREFIX}")
set(_app "${_prefix}/26Viewer.exe")

if(NOT EXISTS "${_app}")
    message(FATAL_ERROR "Deploy: ${_app} was not installed")
endif()

# 1. Qt libraries and plugins.
if(VIEWER_WINDEPLOYQT)
    message(STATUS "Deploy: running windeployqt")
    execute_process(
        COMMAND "${VIEWER_WINDEPLOYQT}" --release --no-translations --no-system-d3d-compiler
                --no-opengl-sw --no-network --skip-plugin-types tls,networkinformation,generic
                --dir "${_prefix}" "${_app}"
        RESULT_VARIABLE _rc OUTPUT_QUIET)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "Deploy: windeployqt failed (${_rc})")
    endif()
endif()

# 2. Everything those binaries link against that is not part of Windows.
list(REMOVE_DUPLICATES VIEWER_DLL_DIRS)
file(GLOB_RECURSE _modules "${_prefix}/*.dll")
file(GLOB _executables "${_prefix}/*.exe")
# DLLs already deployed (e.g. Qt6Core.dll) are found both in the prefix and
# in the toolchain; those "conflicts" are expected and already in place.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${_executables}
    LIBRARIES ${_modules}
    DIRECTORIES ${VIEWER_DLL_DIRS}
    RESOLVED_DEPENDENCIES_VAR _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    CONFLICTING_DEPENDENCIES_PREFIX _conflict
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "[Ss]ystem32" "[Ww]indows/")

set(_copied 0)
foreach(_dll IN LISTS _resolved)
    get_filename_component(_name "${_dll}" NAME)
    if(NOT EXISTS "${_prefix}/${_name}")
        file(COPY "${_dll}" DESTINATION "${_prefix}")
        math(EXPR _copied "${_copied} + 1")
    endif()
endforeach()
message(STATUS "Deploy: copied ${_copied} runtime DLL(s)")

foreach(_dll IN LISTS _unresolved)
    if(NOT _dll MATCHES "^(api-ms|ext-ms)")
        message(WARNING "Deploy: unresolved dependency ${_dll}")
    endif()
endforeach()

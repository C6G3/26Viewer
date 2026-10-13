find_package(Catch2 3 REQUIRED)

add_executable(viewer_tests
    test_bytes.cpp
    test_compression.cpp
    test_image.cpp
    test_blur_pak.cpp
    test_blur_chunks.cpp
    test_blur_animation.cpp
    test_blur_media.cpp
    test_blur_physics.cpp
    test_media.cpp
    test_text.cpp
    test_vfs.cpp
    TestUtil.hpp
)
target_link_libraries(viewer_tests PRIVATE
    viewer::core viewer_game_blur viewer::options
    ZLIB::ZLIB Catch2::Catch2WithMain)

include(Catch)
# PRE_TEST: list tests when ctest runs (DLLs are on PATH via the test preset).
catch_discover_tests(viewer_tests DISCOVERY_MODE PRE_TEST)

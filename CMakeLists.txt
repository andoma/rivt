cmake_minimum_required(VERSION 3.16)
project(rivt VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
pkg_check_modules(XCB REQUIRED xcb)
pkg_check_modules(XCB_KEYSYMS REQUIRED xcb-keysyms)
pkg_check_modules(XCB_ICCCM REQUIRED xcb-icccm)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(XKBCOMMON_X11 REQUIRED xkbcommon-x11)
pkg_check_modules(X11_XCB REQUIRED x11-xcb)
pkg_check_modules(X11 REQUIRED x11)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLESV2 REQUIRED glesv2)

find_package(OpenGL REQUIRED)

add_executable(rivt
    src/main.cpp
    src/core/event_loop.cpp
    src/core/config.cpp
    src/core/util.cpp
    src/core/input_encoder.cpp
    src/core/pane.cpp
    src/core/layout.cpp
    src/core/tab_manager.cpp
    src/core/window.cpp
    src/platform/platform.cpp
    src/platform/x11_backend.cpp
    src/render/font.cpp
    src/render/atlas.cpp
    src/render/renderer.cpp
    src/terminal/vt_parser.cpp
    src/terminal/screen_buffer.cpp
    src/pty/pty.cpp
    src/tmux/tmux_client.cpp
    src/tmux/tmux_controller.cpp
)

target_include_directories(rivt PRIVATE
    src
    ${FREETYPE_INCLUDE_DIRS}
    ${HARFBUZZ_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
    ${XCB_INCLUDE_DIRS}
    ${XCB_KEYSYMS_INCLUDE_DIRS}
    ${XCB_ICCCM_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${XKBCOMMON_X11_INCLUDE_DIRS}
    ${X11_XCB_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLESV2_INCLUDE_DIRS}
)

target_link_libraries(rivt PRIVATE
    ${FREETYPE_LIBRARIES}
    ${HARFBUZZ_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    ${XCB_LIBRARIES}
    ${XCB_KEYSYMS_LIBRARIES}
    ${XCB_ICCCM_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${XKBCOMMON_X11_LIBRARIES}
    ${X11_XCB_LIBRARIES}
    ${X11_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLESV2_LIBRARIES}
    OpenGL::GL
    util
)

target_compile_options(rivt PRIVATE -Wall -Wextra -Wpedantic)

# Debug build: -Og -ggdb -fsanitize=address
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(rivt PRIVATE -Og -ggdb -fsanitize=address)
    target_link_options(rivt PRIVATE -fsanitize=address)
endif()

# Tests
enable_testing()

add_executable(test_vt_parser
    tests/test_vt_parser.cpp
    src/terminal/vt_parser.cpp
)
target_include_directories(test_vt_parser PRIVATE src)
target_compile_options(test_vt_parser PRIVATE -Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(test_vt_parser PRIVATE -Og -ggdb -fsanitize=address)
    target_link_options(test_vt_parser PRIVATE -fsanitize=address)
endif()
add_test(NAME vt_parser COMMAND test_vt_parser)

add_executable(test_screen_buffer
    tests/test_screen_buffer.cpp
    src/terminal/vt_parser.cpp
    src/terminal/screen_buffer.cpp
    src/core/config.cpp
)
target_include_directories(test_screen_buffer PRIVATE src)
target_compile_options(test_screen_buffer PRIVATE -Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(test_screen_buffer PRIVATE -Og -ggdb -fsanitize=address)
    target_link_options(test_screen_buffer PRIVATE -fsanitize=address)
endif()
add_test(NAME screen_buffer COMMAND test_screen_buffer)

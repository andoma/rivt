cmake_minimum_required(VERSION 3.16)
project(rivt VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)

if(APPLE)
    enable_language(OBJCXX)

    find_library(COCOA_FRAMEWORK     Cocoa)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore)
    find_library(OPENGL_FRAMEWORK    OpenGL)
    find_library(CORETEXT_FRAMEWORK  CoreText)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics)
else()
    pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
    pkg_check_modules(XCB REQUIRED xcb)
    pkg_check_modules(XCB_KEYSYMS REQUIRED xcb-keysyms)
    pkg_check_modules(XCB_ICCCM REQUIRED xcb-icccm)
    pkg_check_modules(XCB_XKB REQUIRED xcb-xkb)
    pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
    pkg_check_modules(XKBCOMMON_X11 REQUIRED xkbcommon-x11)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)
    pkg_check_modules(X11 REQUIRED x11)
    pkg_check_modules(EGL REQUIRED egl)
    pkg_check_modules(GLESV2 REQUIRED glesv2)
    find_package(OpenGL REQUIRED)
endif()

set(RIVT_SOURCES
    src/main.cpp
    src/core/config.cpp
    src/core/util.cpp
    src/core/input_encoder.cpp
    src/core/pane.cpp
    src/core/layout.cpp
    src/core/tab_manager.cpp
    src/core/window.cpp
    src/platform/platform.cpp
    src/render/font.cpp
    src/render/atlas.cpp
    src/render/renderer.cpp
    src/terminal/vt_parser.cpp
    src/terminal/screen_buffer.cpp
    src/terminal/kitty_graphics.cpp
    src/terminal/image_store.cpp
    src/pty/pty.cpp
    src/tmux/tmux_client.cpp
    src/tmux/tmux_controller.cpp
)

if(APPLE)
    list(APPEND RIVT_SOURCES
        src/core/event_loop_kqueue.mm
        src/platform/cocoa_backend.mm
    )
else()
    list(APPEND RIVT_SOURCES
        src/core/event_loop.cpp
        src/platform/x11_backend.cpp
    )
endif()

add_executable(rivt ${RIVT_SOURCES})

target_include_directories(rivt PRIVATE
    src
    src/third_party
    ${FREETYPE_INCLUDE_DIRS}
    ${HARFBUZZ_INCLUDE_DIRS}
)

if(APPLE)
    target_compile_definitions(rivt PRIVATE GL_SILENCE_DEPRECATION)
    set_source_files_properties(
        src/core/event_loop_kqueue.mm
        src/platform/cocoa_backend.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
else()
    target_include_directories(rivt PRIVATE
        ${FONTCONFIG_INCLUDE_DIRS}
        ${XCB_INCLUDE_DIRS}
        ${XCB_KEYSYMS_INCLUDE_DIRS}
        ${XCB_ICCCM_INCLUDE_DIRS}
        ${XCB_XKB_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
        ${XKBCOMMON_X11_INCLUDE_DIRS}
        ${X11_XCB_INCLUDE_DIRS}
        ${X11_INCLUDE_DIRS}
        ${EGL_INCLUDE_DIRS}
        ${GLESV2_INCLUDE_DIRS}
    )
endif()

target_link_libraries(rivt PRIVATE
    ${FREETYPE_LIBRARIES}
    ${HARFBUZZ_LIBRARIES}
)

target_link_directories(rivt PRIVATE
    ${FREETYPE_LIBRARY_DIRS}
    ${HARFBUZZ_LIBRARY_DIRS}
)

if(APPLE)
    target_link_libraries(rivt PRIVATE
        ${COCOA_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${OPENGL_FRAMEWORK}
        ${CORETEXT_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${COREGRAPHICS_FRAMEWORK}
    )
else()
    target_link_libraries(rivt PRIVATE
        ${FONTCONFIG_LIBRARIES}
        ${XCB_LIBRARIES}
        ${XCB_KEYSYMS_LIBRARIES}
        ${XCB_ICCCM_LIBRARIES}
        ${XCB_XKB_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
        ${XKBCOMMON_X11_LIBRARIES}
        ${X11_XCB_LIBRARIES}
        ${X11_LIBRARIES}
        ${EGL_LIBRARIES}
        ${GLESV2_LIBRARIES}
        OpenGL::GL
        util
    )
endif()

target_compile_options(rivt PRIVATE -Wall -Wextra -Wpedantic)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(rivt PRIVATE -Og -ggdb -fsanitize=address)
    target_link_options(rivt PRIVATE -fsanitize=address)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(rivt PRIVATE -O2 -g -fno-omit-frame-pointer)
endif()

if(APPLE)
    set_target_properties(rivt PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/data/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "rivt"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.rivt.terminal"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
endif()

# Install
include(GNUInstallDirs)
if(APPLE)
    install(TARGETS rivt BUNDLE DESTINATION .)
else()
    install(TARGETS rivt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES data/rivt.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES data/rivt.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
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
    src/terminal/kitty_graphics.cpp
    src/terminal/image_store.cpp
    src/core/config.cpp
    src/core/util.cpp
)
target_include_directories(test_screen_buffer PRIVATE src src/third_party)
target_compile_options(test_screen_buffer PRIVATE -Wall -Wextra -Wpedantic)
if(APPLE)
    target_compile_definitions(test_screen_buffer PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(test_screen_buffer PRIVATE ${OPENGL_FRAMEWORK})
else()
    target_link_libraries(test_screen_buffer PRIVATE OpenGL::GL)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(test_screen_buffer PRIVATE -Og -ggdb -fsanitize=address)
    target_link_options(test_screen_buffer PRIVATE -fsanitize=address)
endif()
add_test(NAME screen_buffer COMMAND test_screen_buffer)

add_executable(test_snapshot
    tests/test_snapshot.cpp
    src/proto/snapshot.cpp
    src/terminal/vt_parser.cpp
    src/terminal/screen_buffer.cpp
    src/terminal/kitty_graphics.cpp
    src/terminal/image_store.cpp
    src/core/config.cpp
    src/core/util.cpp
)
target_include_directories(test_snapshot PRIVATE src src/third_party)
target_compile_options(test_snapshot PRIVATE -Wall -Wextra -Wpedantic)
if(APPLE)
    target_compile_definitions(test_snapshot PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(test_snapshot PRIVATE ${OPENGL_FRAMEWORK})
else()
    target_link_libraries(test_snapshot PRIVATE OpenGL::GL)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(test_snapshot PRIVATE -Og -ggdb -fsanitize=address)
    target_link_options(test_snapshot PRIVATE -fsanitize=address)
endif()
add_test(NAME snapshot COMMAND test_snapshot)

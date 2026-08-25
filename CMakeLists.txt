cmake_minimum_required(VERSION 3.16)
project(rivt VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

# The UI needs freetype/xcb/GL; headless machines get rivtd only.
# Autodetected: if freetype2 is absent, the UI is skipped. Force with
# -DRIVT_UI=ON/OFF.
if(NOT DEFINED RIVT_UI)
    pkg_check_modules(FREETYPE_PROBE QUIET freetype2)
    if(FREETYPE_PROBE_FOUND OR APPLE)
        set(RIVT_UI ON)
    else()
        set(RIVT_UI OFF)
        message(STATUS "freetype2 not found — building rivtd only (set -DRIVT_UI=ON to require the UI)")
    endif()
endif()

# QUIC transport (rivtd <-> rivt across machines). Fetches pinned
# picoquic + picotls at configure time; needs OpenSSL.
option(RIVT_QUIC "Build QUIC remote transport" ON)
if(RIVT_QUIC AND NOT APPLE)
    include(FetchContent)
    set(PICOQUIC_FETCH_PTLS Y CACHE BOOL "" FORCE)
    set(PICOQUIC_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(picoquic
        GIT_REPOSITORY https://github.com/private-octopus/picoquic.git
        GIT_TAG d199d45be8690dfa8653e214733243524112e3c0
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(picoquic)
    find_package(OpenSSL REQUIRED)
endif()

if(RIVT_UI)
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
endif() # RIVT_UI

if(RIVT_UI)
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
    src/remote/remote_client.cpp
    src/remote/remote_controller.cpp
    src/proto/snapshot.cpp
    src/net/identity.cpp
    src/net/quic_engine.cpp
    src/net/stun.cpp
    src/net/rendezvous.cpp
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
    ${picoquic_SOURCE_DIR}/picoquic
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
    picoquic-core
    OpenSSL::SSL
    OpenSSL::Crypto
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
endif() # RIVT_UI

if(RIVT_UI AND APPLE)
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
if(RIVT_UI)
    if(APPLE)
        install(TARGETS rivt BUNDLE DESTINATION .)
    else()
        install(TARGETS rivt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES data/rivt.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES data/rivt.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    endif()
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
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(test_snapshot PRIVATE -Og -ggdb -fsanitize=address)
    target_link_options(test_snapshot PRIVATE -fsanitize=address)
endif()
add_test(NAME snapshot COMMAND test_snapshot)

if(NOT APPLE)
add_executable(rivtd
    src/daemon/rivtd.cpp
    src/core/config.cpp
    src/core/util.cpp
    src/core/layout.cpp
    src/core/pane.cpp
    src/core/event_loop.cpp
    src/proto/snapshot.cpp
    src/terminal/vt_parser.cpp
    src/terminal/screen_buffer.cpp
    src/terminal/kitty_graphics.cpp
    src/terminal/image_store.cpp
    src/pty/pty.cpp
    src/net/identity.cpp
    src/net/quic_engine.cpp
    src/net/stun.cpp
    src/net/rendezvous.cpp
)
target_include_directories(rivtd PRIVATE src src/third_party ${picoquic_SOURCE_DIR}/picoquic)
target_compile_options(rivtd PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rivtd PRIVATE util picoquic-core OpenSSL::SSL OpenSSL::Crypto)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(rivtd PRIVATE -Og -ggdb -fsanitize=address)
    target_link_options(rivtd PRIVATE -fsanitize=address)
endif()
install(TARGETS rivtd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(data/rivtd.service.in rivtd.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rivtd.service
        DESTINATION lib/systemd/user)
endif()

if(NOT APPLE)
add_test(NAME daemon COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/test_daemon.py $<TARGET_FILE:rivtd>)
endif()

if(RIVT_QUIC AND NOT APPLE)
add_executable(test_quic
    tests/test_quic.cpp
    src/net/identity.cpp
    src/net/quic_engine.cpp
    src/net/stun.cpp
    src/core/event_loop.cpp
)
target_include_directories(test_quic PRIVATE src ${picoquic_SOURCE_DIR}/picoquic)
target_compile_options(test_quic PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_quic PRIVATE picoquic-core OpenSSL::Crypto)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(test_quic PRIVATE -Og -ggdb -fsanitize=address)
    target_link_options(test_quic PRIVATE -fsanitize=address)
endif()
add_test(NAME quic COMMAND test_quic)
endif()

if(RIVT_QUIC AND NOT APPLE)
add_executable(test_ws
    tests/test_ws.cpp
    src/net/ws_client.cpp
    src/core/event_loop.cpp
)
target_include_directories(test_ws PRIVATE src)
target_compile_options(test_ws PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_ws PRIVATE OpenSSL::SSL OpenSSL::Crypto)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(test_ws PRIVATE -Og -ggdb -fsanitize=address)
    target_link_options(test_ws PRIVATE -fsanitize=address)
endif()
add_test(NAME ws COMMAND test_ws)
endif()

if(RIVT_QUIC AND NOT APPLE)
add_executable(test_membership
    tests/test_membership.cpp
    src/net/identity.cpp
    src/net/membership.cpp
    src/net/rendezvous.cpp
)
target_include_directories(test_membership PRIVATE src)
target_compile_options(test_membership PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_membership PRIVATE OpenSSL::SSL OpenSSL::Crypto)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(test_membership PRIVATE -Og -ggdb -fsanitize=address)
    target_link_options(test_membership PRIVATE -fsanitize=address)
endif()
add_test(NAME membership COMMAND test_membership)
endif()

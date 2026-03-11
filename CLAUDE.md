# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

rivt is a modern Linux terminal emulator written in C++20 with first-class tmux control mode (`-CC`) integration. It renders tmux panes as native windows/panes with proper scrollback, search, and clipboard. Also works as a standalone terminal.

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug .   # Debug includes ASan
cmake --build build
```

Debug builds automatically enable `-fsanitize=address -Og -ggdb`. Release builds use standard optimization.

## Test

```bash
cd build && ctest --output-on-failure
# Or run individually:
./build/test_vt_parser
./build/test_screen_buffer
```

Tests exist for VT parser and screen buffer only. No linter or formatter is configured; compiler warnings (`-Wall -Wextra -Wpedantic`) serve as the main quality check.

## Dependencies

All external deps are found via pkg-config: freetype2, harfbuzz, fontconfig, xcb (+ keysyms, icccm), xkbcommon, OpenGL (GLX/EGL), GLESV2.

## Architecture

**Single-threaded, epoll-driven event loop** — all I/O (PTY reads, tmux subprocess, X11 events, timers) multiplexed through `EventLoop`. Main loop in `main.cpp` polls with 0ms timeout when a render is needed, 16ms otherwise.

### Module layout (`src/`)

- **`core/`** — Window management, pane abstraction, tab manager, split layout engine, input encoding, event loop, config, types
- **`platform/`** — Abstract `Platform` interface with X11/xcb backend implementation (Wayland planned but not started)
- **`render/`** — OpenGL 3.3+ renderer with FreeType/HarfBuzz font shaping and shelf-packed glyph atlas (mask, LCD subpixel, color emoji)
- **`terminal/`** — VT100 state machine parser (Paul Williams model) and screen buffer (ring-buffer scrollback, dirty tracking, selection)
- **`pty/`** — forkpty() wrapper for local shell spawning
- **`tmux/`** — TmuxClient (subprocess I/O, line protocol parsing) and TmuxController (state sync, layout parsing, input routing)

### Key design patterns

- **Unified pane model**: Both local PTY and tmux-remote panes share the same `ScreenBuffer` + `VtParser`. Tmux panes override the write callback to route output through `send-keys`.
- **Layout engine**: Binary tree of splits for standalone mode; tmux mode parses tmux's recursive checksum layout strings to reconstruct pane geometry.
- **Callback-based VtParser**: Parser dispatches through a `VtHandler` interface — `ScreenBuffer` implements it. Supports Kitty keyboard protocol, SGR mouse, extended colors, OSC sequences.
- **Renderer dirty tracking**: Lines marked dirty on mutation; renderer only re-uploads changed lines. Two-pass rendering (backgrounds, then glyphs).
- **Tab manager**: One tab per tmux window or local window group. Tab bar rendered by the renderer.

### Main data structures (in `core/types.h`)

- **Cell** (16 bytes): codepoint + fg/bg colors + attributes
- **Line**: vector of Cells + wrapped/dirty flags
- **Pane**: ScreenBuffer + VtParser + Pty + pixel rect + write callback

### tmux integration flow

TmuxClient spawns `tmux -CC` subprocess → parses `%output`, `%layout-change`, `%window-*` notifications → TmuxController mirrors session/window/pane topology as local tabs and panes → keyboard input sent back via `send-keys` commands.

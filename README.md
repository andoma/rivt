# rivt

A modern Linux terminal emulator written in C++20 with first-class tmux integration.

## Highlights

- **Splits, tabs, and windows** — vertical/horizontal split panes, tabbed windows, and multi-window support built in
- **Inline search** — search scrollback with highlighted matches, navigate between results with keyboard shortcuts
- **Native tmux control mode** — connects via `tmux -CC` and renders tmux panes, tabs, and splits as native UI elements with proper scrollback, search, and clipboard
- **GPU-accelerated rendering** — OpenGL 3.3+ renderer with FreeType/HarfBuzz font shaping, LCD subpixel antialiasing, and color emoji support
- **Inline images** — Kitty graphics protocol for displaying images directly in the terminal
- **Fast and lightweight** — single-threaded, epoll-driven event loop with dirty-line tracking so only changed content is re-rendered
- **Kitty keyboard protocol** — full support for advanced key reporting used by modern TUI applications
- **Clipboard integration** — OSC 52 with Kitty extended clipboard protocol for text and image clipboard access

## Build

Dependencies (install via your package manager):

```
freetype2 harfbuzz fontconfig
xcb xcb-keysyms xcb-icccm xkbcommon xkbcommon-x11 x11-xcb x11
egl glesv2
```

On Ubuntu/Debian:

```bash
sudo apt install build-essential cmake ninja-build \
  libfreetype-dev libharfbuzz-dev libfontconfig-dev \
  libxcb1-dev libxcb-keysyms1-dev libxcb-icccm4-dev \
  libxkbcommon-dev libxkbcommon-x11-dev libx11-xcb-dev libx11-dev \
  libegl-dev libgles2-mesa-dev libgl-dev
```

Build:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release .
cmake --build build
```

## Install

```bash
sudo ninja -C build install
```

This installs `rivt` to `/usr/local/bin` by default. To change the prefix:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr .
```

### Set as default terminal (Debian/Ubuntu)

```bash
sudo update-alternatives --install /usr/bin/x-terminal-emulator x-terminal-emulator /usr/local/bin/rivt 50
sudo update-alternatives --set x-terminal-emulator /usr/local/bin/rivt
```

## Usage

Standalone terminal:

```bash
rivt
```

Connect to a tmux session:

```bash
rivt tmux -CC new -A -s main
```

## License

MIT

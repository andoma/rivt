# rivt — TODO

## High Priority

- [ ] **Config file**: TOML or similar parser, load `~/.config/rivt/config` at startup, support live reload
- [ ] **URL detection**: regex-based URL detection, underline on hover, Ctrl+click to open via xdg-open
- [ ] **Hyperlinks (OSC 8)**: store URI per cell, render underline, click handler
- [ ] **Bell**: visual bell flash and/or audio bell (configurable)

## Medium Priority

- [ ] **DPI awareness**: query actual monitor DPI, scale fonts and padding accordingly
- [ ] **Grapheme cluster rendering**: use HarfBuzz cluster info to correctly render multi-codepoint graphemes
- [ ] **Scroll indicator**: visual marker showing position within scrollback
- [ ] **OSC 133 prompt zones**: track zone boundaries in screen buffer for jump-to-prompt navigation
- [ ] **Paste safety**: warn on multi-line paste containing control characters

## Low Priority

- [ ] **Wayland backend**: wl_surface + xdg_shell + wp_viewporter, share renderer with X11
- [ ] **Sixel graphics**: image display via Sixel escape sequences
- [ ] **Font ligatures**: discretionary ligature support for coding fonts
- [ ] **Session picker UI**: list tmux sessions, attach/detach
- [ ] **Background transparency / blur**: compositor-dependent alpha blending
- [ ] **Synchronized rendering**: DCS = 1s / 2s to reduce flicker during rapid updates

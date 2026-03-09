# rivt — TODO

Audit of `plan.md` vs codebase as of 2026-03-09.

## Status Summary

| Plan Section | Topic | Status |
|---|---|---|
| 1 – Platform | X11 backend (EGL, input, clipboard) | Done |
| 1 – Platform | Wayland backend | Not started |
| 2 – Font | FreeType + HarfBuzz + fontconfig pipeline | Done |
| 2 – Font | Glyph atlas (shelf packing, mask/lcd/color) | Done |
| 3 – Render | OpenGL unified shader, cursor, attributes | Done |
| 4a – VT Parser | State machine (UTF-8, CSI, OSC, ESC, DCS) | Done |
| 4b – Screen | Grid, scrollback, alt screen, scroll regions, dirty tracking | Done |
| 4c – PTY | forkpty, non-blocking I/O, resize | Done |
| 4d – Event loop | epoll + timerfd | Done |
| 4e – Keyboard | Kitty protocol (push/pop/query), legacy encoding | Done |
| 4f – Mouse | Mode parsing + forwarding (1000/1002/1003/1006) | Done |
| 4g – OSC 133 | Prompt zone markers | Partial — recognized, zones not tracked |
| 4h – OSC 52 | Clipboard read/write | Partial — write callback exists, not fully wired |
| 5a – Config | 256-color palette, defaults | Done |
| 5b – Config file | TOML/conf parsing, reload | Not started |
| 5c – Scrollback nav | Mouse wheel, Shift+PageUp/Down | Done |
| 5d – Font size | Ctrl+Shift+±/0 | Done |
| 5e – Selection | Mouse selection, Ctrl+Shift+C | Done |
| 5f – Search | Scrollback search | Not started |
| 5g – Focus reporting | DECSET 1004 | Done |
| 5h – Paste | Ctrl+Shift+V, bracketed paste | Done |
| 5i – Grapheme clusters | Extended grapheme rendering | Partial — mode flag tracked, not rendered |
| 5j – DPI awareness | Per-monitor scaling | Partial — stub returns 1.0 |
| 6 – tmux -CC | Control-mode integration | Not started |
| 7 – Tabs/Panes | Splits, tab bar | Not started |
| 8a – Bell | Visual/audio notification | Not started |
| 8b – Search UI | Search bar overlay | Not started |
| 8c – Scroll indicator | Position indicator in scrollback | Not started |
| 8d – URL detection | Clickable URLs | Not started |
| 8e – Hyperlinks | OSC 8 | Not started |
| 8f – Shell integration | Jump-to-prompt navigation | Not started |

---

## TODO Items

### High Priority — finish partially-implemented features

- [x] **Mouse event forwarding**: wire parsed mouse events through to PTY (SGR 1006 encoding)
- [x] **Selection model**: click-drag, double-click word, triple-click line; Ctrl+Shift+C to CLIPBOARD, auto PRIMARY
- [ ] **OSC 52 clipboard**: complete read/write wiring to X11 clipboard
### Medium Priority — new features that round out the terminal

- [ ] **Search**: scrollback search with incremental highlight (Ctrl+Shift+F)
- [ ] **URL detection**: regex-based URL detection, underline on hover, click-to-open
- [ ] **Hyperlinks (OSC 8)**: store URI per cell, render underline, click handler
- [ ] **Bell**: visual bell flash and/or audio bell (configurable)
- [ ] **DPI awareness**: query actual monitor DPI from X11/Wayland, scale fonts & padding
- [ ] **Grapheme cluster rendering**: use HarfBuzz cluster info to correctly render multi-codepoint graphemes
- [ ] **Scroll indicator**: visual indicator showing position within scrollback
- [ ] **OSC 133 prompt zones**: track zone boundaries in screen buffer for navigation
- [ ] **Shell integration nav**: jump to previous/next prompt (Ctrl+Shift+Up/Down) using OSC 133 zones

### Low Priority — larger efforts / can defer

- [ ] **Wayland backend**: wl_surface + xdg_shell + wp_viewporter; share renderer with X11
- [ ] **tmux -CC integration**: control-mode protocol parser, virtual panes, tmux session attach
- [ ] **Tabs / pane splits**: tab bar, horizontal/vertical splits, focus cycling
- [ ] **Config file**: add TOML (or similar) parser; load `~/.config/rivt/config` at startup; support live reload

---

*Generated from audit of plan.md against codebase.*

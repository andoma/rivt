# rivt — A Modern Linux Terminal

A native Linux terminal emulator with first-class tmux control mode (`-CC`) integration, rendering tmux panes as native windows/panes with proper scrollback, search, and clipboard integration. Standalone terminal by default, tmux superpower when you want it.

## 1. Project Bootstrapping

- Build system: CMake
- Dependencies (system, via pkg-config/find_package):
  - freetype2, harfbuzz, fontconfig
  - OpenGL (GLX for X11, EGL/GLES for Wayland)
  - xkbcommon (keyboard handling, shared across both backends)
  - xcb + xcb-keysyms + xcb-icccm (X11 backend)
  - libwayland-client + wayland-protocols + xdg-shell (Wayland backend, phase 2)
- Configuration file format (TOML or similar)
- Source layout:
  - `src/platform/` — platform abstraction interface + backends
  - `src/render/` — glyph atlas, font loading, GL pipeline
  - `src/terminal/` — VT parser, screen buffer, scrollback
  - `src/tmux/` — -CC protocol parser and state model
  - `src/pty/` — standalone PTY management
  - `src/core/` — pane/tab/window model, config, event loop (epoll)

## 2. Platform Abstraction Layer

Abstract interface that both X11 and Wayland backends implement:

- **Window management**: create, destroy, resize, set title, fullscreen toggle
- **GL context**: create/bind context, swap buffers (GLX vs EGL)
- **Input — keyboard**: key press/release events normalized to xkbcommon keysyms + UTF-8 text, modifier state, compose/dead key handling, IME
- **Input — mouse**: button, motion, scroll events in pixel coords
- **Clipboard**: get/set for both PRIMARY and CLIPBOARD selections
- **Display info**: DPI / scale factor, monitor geometry
- **Event fd**: expose a pollable fd for integration into the epoll event loop

### Phase 1: X11 backend (xcb)
- Window via xcb_create_window, GL via GLX or EGL/X11
- Keyboard via xkbcommon-x11
- Clipboard via xcb selections (CLIPBOARD + PRIMARY atoms)
- DPI via Xrdb or xrandr

### Phase 2: Wayland backend
- Window via wl_surface + xdg_toplevel
- GL via EGL/Wayland
- Keyboard via wl_keyboard + xkbcommon
- Clipboard via wl_data_device + zwp_primary_selection
- DPI via wl_output scale factor

## 3. Text Rendering Pipeline

### 3a. Font Management
- Font discovery via fontconfig: query for monospace, resolve family/size/style
- Fallback chain: primary font → CJK fallback → symbol fallback → emoji font (e.g. Noto Color Emoji)
- Load each font via FreeType, create corresponding HarfBuzz hb_font_t from FT_Face
- Cell sizing: measure advance width from primary font (U+0030 or U+004D), line height from ascender − descender + line gap
- Bold/italic: prefer distinct font files from fontconfig (e.g. "JetBrains Mono Bold"), fall back to FT_EMBOLDEN / FT_OBLIQUE synthetic styles
- Font size change at runtime: invalidate atlas, recompute cell size, resize grid

### 3b. Glyph Rasterization & Shaping
- Simple path (ASCII, Latin, most monospace): codepoint → FT_Get_Char_Index → FT_Load_Glyph → FT_Render_Glyph
- Walk fallback chain when FT_Get_Char_Index returns 0
- Complex path (emoji, ZWJ sequences, combining chars): feed grapheme cluster through HarfBuzz shaping → get positioned glyph(s)
- Emoji: use FT_LOAD_COLOR flag → BGRA bitmap instead of grayscale
- Wide characters (CJK, emoji): occupy 2 cells, wcwidth() or ICU for width determination
- Render mode (configurable, auto-detect default):
  - **Grayscale**: FT_RENDER_MODE_NORMAL — single-channel alpha mask, simple blending
  - **LCD subpixel**: FT_RENDER_MODE_LCD — 3x wide bitmap, per-channel alpha (R/G/B)
  - **Auto**: query fontconfig FC_RGBA for subpixel order; use LCD on low-DPI (<= ~144), grayscale on HiDPI
- LCD subpixel details:
  - Subpixel order detection via fontconfig (RGB, BGR, VRGB, VBGR, none)
  - LCD filter: FT_Library_SetLcdFilter(FT_LCD_FILTER_DEFAULT)
  - Glyphs stored 3x wide in atlas, tagged as `lcd` type

### 3c. Glyph Atlas
- Single RGBA texture atlas (unified for all glyph types)
- Three glyph types, tagged per entry:
  - `mask`: grayscale — single-channel alpha, multiplied by fg color in shader
  - `lcd`: subpixel — 3 channel alpha (R/G/B), per-channel blending with fg/bg
  - `color`: emoji — BGRA sampled directly
- Atlas entry: { u, v, w, h, bearing_x, bearing_y, glyph_type }
- LCD glyphs stored 3x wide in atlas (atlas width accounts for this)
- Lookup key: hash(font_index, glyph_id, size, render_mode)
- Atlas packing: shelf packing (sufficient for fixed-height terminal glyphs)
- Eviction/regrowth: resize texture and re-pack when full (rare with typical terminal usage)

### 3d. Rendering
- OpenGL 3.3+ core profile (GLX on X11, EGL on Wayland)
- Two-pass draw per frame:
  1. Background pass: draw colored quads for all cells with non-default bg
  2. Glyph pass: instanced/batched quads, one per cell, sampling from atlas
- Fragment shader handles all three glyph types:
  - `mask`: sample single alpha, multiply by fg color, standard alpha blend
  - `lcd`: sample per-channel alpha, dual-pass blend (GL_ONE / GL_ONE_MINUS_SRC_COLOR) — requires bg already in framebuffer from pass 1
  - `color`: sample RGBA directly (emoji), standard alpha blend
- Cell attributes: fg/bg color (24-bit), bold, italic, underline, strikethrough, inverse, dim
- Underline / strikethrough: draw as thin quads at appropriate y offset
- Cursor: colored quad (block/bar/underline), XOR or overlay blending, blink via timer
- Selection highlight: overlay colored quad on selected cell range
- Dirty region tracking: only rebuild vertex data for changed lines

## 4. VT Parser / Terminal Emulation

### 4a. Parser Architecture
- State machine based on Paul Faulkner Williams' VT parser model
- UTF-8 only: decode incoming bytes into codepoints at the front of the pipeline, no legacy character set support (G0/G1/G2/G3, ISO 2022)
- Callback/visitor interface — parser calls methods on a handler, zero knowledge of screen state:
  - `print(uint32_t codepoint)` — printable character
  - `print_grapheme(const uint32_t *cps, int len)` — multi-codepoint grapheme cluster
  - `execute(uint8_t code)` — C0 control codes
  - `csi_dispatch(const CsiParams &params, char intermediate, char final_byte)`
  - `osc_dispatch(int command, std::string_view payload)`
  - `dcs_start()` / `dcs_put(uint8_t byte)` / `dcs_end()` — DCS passthrough (tmux -CC detection)
  - `esc_dispatch(char intermediate, char final_byte)` — simple ESC sequences
- Parser states: Ground, Escape, EscapeIntermediate, CsiEntry, CsiParam, CsiIntermediate, CsiIgnore, OscString, DcsEntry, DcsPassthrough, DcsIgnore, Utf8Decode
- CsiParams: support for sub-parameters separated by `:` (needed for SGR extended colors and kitty keyboard)

### 4b. Supported Sequences

**C0 Controls:**
- BEL (0x07), BS (0x08), HT (0x09), LF (0x0A), VT (0x0B), FF (0x0C), CR (0x0D)

**CSI — Cursor & Editing:**
- CUU/CUD/CUF/CUB (A/B/C/D) — movement
- CUP/HVP (H/f) — absolute position
- ED (J) — erase in display (0=below, 1=above, 2=all, 3=scrollback)
- EL (K) — erase in line
- ECH (X) — erase characters
- ICH (@ ) — insert characters
- DCH (P) — delete characters
- IL/DL (L/M) — insert/delete lines
- SU/SD (S/T) — scroll up/down
- DECSTBM (r) — set scrolling region
- DECSC/DECRC (ESC 7 / ESC 8) — save/restore cursor

**CSI — SGR (m) — Text Attributes:**
- Basic attributes: bold, dim, italic, underline, blink, inverse, hidden, strikethrough
- Reset: 0, per-attribute resets
- Colors: 8-color (30-37/40-47), bright (90-97/100-107), 256-color (38;5;N / 48;5;N), truecolor (38;2;R;G;B / 48;2;R;G;B)
- Underline styles via colon sub-params: single, double, curly, dotted, dashed (CSI 4:N m)
- Underline color (CSI 58:2::R:G:B m)

**CSI — Modes (DECSET/DECRST, CSI ? N h/l):**
- 1 — application cursor keys
- 12 — cursor blink
- 25 — cursor visibility
- 1004 — focus in/out events
- 1049 — alternate screen buffer (save cursor + switch + clear)
- 2004 — bracketed paste
- 2027 — grapheme cluster mode

**CSI — Mouse Reporting:**
- 1000 — basic mouse reporting
- 1002 — button-event tracking (drag)
- 1003 — any-event tracking (all motion)
- 1006 — SGR mouse encoding (modern, no 223-column limit)

**CSI — Kitty Keyboard Protocol:**
- CSI ? u — query current flags
- CSI > N u — push keyboard mode flags onto stack
- CSI < N u — pop keyboard mode(s) from stack
- Key encoding: CSI keycode ; modifiers u, with event types (press/repeat/release) via sub-params
- Report modifier keys as distinct events, disambiguate keys that legacy encoding conflates

**OSC Sequences (terminated by ST or BEL):**
- OSC 0 / 1 / 2 — set icon name / window title
- OSC 7 — current working directory (used by shells for smart CWD tracking)
- OSC 8 — hyperlinks: OSC 8 ; params ; URI ST ... OSC 8 ; ; ST
- OSC 52 — clipboard access: read/write system clipboard (c/p/s selections)
- OSC 10/11/12 — query/set foreground/background/cursor color
- OSC 133 — shell integration / semantic prompt zones (command start/end markers)

**DCS:**
- tmux -CC detection: \033P1000p
- Passthrough to tmux protocol parser when in -CC mode

### 4c. What We Skip
- VT52 mode
- G0/G1/G2/G3 character set designation and invocation (ISO 2022)
- DECOM (origin mode), conformance levels
- C1 control codes as 8-bit (only accept ESC-prefixed 7-bit forms)
- Sixel graphics (stretch goal)
- ReGIS

## 5. Screen Buffer

### 5a. Cell Storage
- Cell struct (compact, cache-friendly for the common ASCII case):
  ```
  struct Cell {
      uint32_t codepoint;   // Unicode codepoint, or GRAPHEME_SENTINEL for multi-codepoint clusters
      uint32_t fg;          // 24-bit RGB in low bits, flags/palette index in high byte
      uint32_t bg;          // same encoding
      uint16_t attrs;       // bitfield: bold, dim, italic, underline style (3 bits), strikethrough,
                            //   inverse, hidden, wide, wide_continuation, wrap, hyperlink flag
  };
  ```
- Grapheme cluster table: side table `std::vector<std::u32string>` indexed by (codepoint - GRAPHEME_SENTINEL_BASE) for multi-codepoint cells (emoji ZWJ sequences, combining characters)
- Hyperlink storage: separate table mapping hyperlink ID → (URI, params), cells reference by ID in attrs or a parallel array
- Color encoding: support both palette indices (0-255) and truecolor in the same 32-bit field, distinguished by a flag bit

### 5b. Grid / Line Structure
- Line struct:
  ```
  struct Line {
      std::vector<Cell> cells;
      bool wrapped;             // true if this line is a continuation of the previous (soft wrap)
      bool dirty;               // changed since last render
      uint32_t semantic_zone;   // OSC 133 prompt/command/output zone marker
  };
  ```
- Active screen: `cols × rows` grid of Lines, sized to match terminal dimensions
- Alternate screen: separate grid, swapped in/out via DECSET 1049, no scrollback

### 5c. Scrollback Ring Buffer
- Ring buffer of Lines above the visible viewport
- Configurable max size (default ~10,000 lines, user-settable)
- When a line scrolls off the top of the visible area, push it into the ring buffer
- Viewport offset: signed offset from the bottom (0 = live, negative = scrolled back)
- Memory: at 14 bytes/cell × 200 cols × 10,000 lines ≈ 28 MB — reasonable, but consider compressing or dropping attributes on old lines if memory becomes a concern

### 5d. Resize / Reflow
- On terminal resize (SIGWINCH from PTY, or layout change from tmux):
  - Recompute rows × cols from pixel size / cell size
  - Reflow wrapped lines: unwrap soft-wrapped lines into their logical line, then re-wrap to new width
  - Non-wrapped lines: truncate or pad, no reflow
  - Cursor position: track cursor by logical line offset, recompute row/col after reflow
  - Scrollback lines also reflow (important for search consistency)
- Alternate screen: no reflow, just truncate/pad (standard behavior)

### 5e. Selection Model
- Two modes:
  - **Stream selection**: start cell → end cell, wraps across lines (normal click-drag)
  - **Rectangular / block selection**: column range × row range (Alt+click-drag)
- Selection anchored to buffer coordinates (scrollback-aware), survives scrolling
- Snap modes: character, word (double-click), line (triple-click)
- Word boundary detection: Unicode-aware, configurable word separators
- Selection stored as (start_row, start_col, end_row, end_col, mode)
- On selection complete: copy to PRIMARY selection immediately (X11 convention), explicit Ctrl+Shift+C for CLIPBOARD
- Selection invalidation: if underlying buffer content changes (new output in selected region), either preserve or clear — configurable

### 5f. Search
- Incremental search over scrollback + visible content
- Operate on logical lines (unwrap soft-wrapped lines before matching)
- Substring match only (Boyer-Moore or similar, no regex dependency)
- Match highlighting: overlay rendered on matching cells, distinct from selection
- Navigate matches: next/previous, with wraparound
- Search scope: current pane, optionally all panes
- Integration with OSC 133 zones: "search within last command output" as a convenience feature

## 6. tmux -CC Protocol Layer

### 6a. Connection Lifecycle
- Spawn tmux via `forkpty()` or `pipe()`+`fork()`+`exec()`:
  - New session: `tmux -CC new-session`
  - Attach existing: `tmux -CC attach -t <session>`
- Detect control mode entry via DCS sequence `\033P1000p` from the VT parser
- When DCS detected, switch the byte stream from VT parsing to tmux line-protocol parsing
- On disconnect / `%exit`: clean up pane state, optionally reconnect or fall back to standalone mode
- Detach: send empty line (just `\n`) on stdin, or `detach-client` command

### 6b. Line Protocol Parser
- Read lines from tmux stdout, delimited by `\n`
- **Guarded command responses**:
  - `%begin <timestamp> <cmd_number> <flags>` — start of command output
  - `%end <timestamp> <cmd_number>` — success
  - `%error <timestamp> <cmd_number>` — failure
  - Collect output between begin/end, dispatch to command callback
- **Notifications** (asynchronous, not in response to a command):
  - `%output <pane_id> <escaped_data>` — pane output (basic flow control)
  - `%extended-output <pane_id> <age> <data> ... :` — pane output (with flow control enabled)
  - `%window-add <window_id>`
  - `%window-close <window_id>`
  - `%window-renamed <window_id> <name>`
  - `%window-pane-changed <window_id> <pane_id>`
  - `%layout-change <window_id> <layout_string>`
  - `%session-changed <session_id> <name>`
  - `%session-renamed <name>`
  - `%sessions-changed`
  - `%pane-mode-changed <pane_id>`
  - `%pause <pane_id>`
  - `%continue <pane_id>`
  - `%exit [reason]`
- **Output unescaping**: tmux escapes bytes < 0x20 and `\` as octal (`\134` for `\`), decode before feeding to pane's VT parser

### 6c. Command Interface
- Send commands to tmux by writing to its stdin, one per line
- Commands return responses inside `%begin`/`%end` blocks
- Track pending commands by number for matching responses to requests
- Key commands:
  - `new-window [-n name]`
  - `split-window [-h|-v] [-t target]`
  - `kill-pane [-t target]`
  - `kill-window [-t target]`
  - `resize-pane [-t target] [-x width] [-y height]`
  - `select-pane [-t target]`
  - `select-window [-t target]`
  - `send-keys [-t target] <keys>` — forward keyboard input to pane
  - `capture-pane [-t target] [-p] [-S start] [-E end]` — grab pane content (for sync after pause)
  - `list-windows [-F format]`
  - `list-panes [-F format]`
  - `list-sessions [-F format]`
  - `refresh-client -C <cols>,<rows>` — report client size
  - `refresh-client -f pause-after=1` — enable flow control
  - `refresh-client -A %<pane_id>` — continue a paused pane

### 6d. Flow Control
- Enable via `refresh-client -f pause-after=<seconds>` after attach
- When enabled, `%output` is replaced by `%extended-output`
- On `%pause <pane_id>`: stop expecting output for that pane, render stale content
- When ready to consume more: `refresh-client -A %<pane_id>` to continue
- On `%continue <pane_id>`: output resumes, optionally `capture-pane` to sync if output was missed
- Essential for handling fast output (e.g. `cat` large file) without overwhelming the client

### 6e. State Model
- Mirror tmux's session/window/pane topology locally:
  ```
  struct TmuxPane { int id; ScreenBuffer buffer; int x, y, w, h; bool paused; };
  struct TmuxWindow { int id; std::string name; std::vector<TmuxPane> panes; std::string layout; };
  struct TmuxSession { int id; std::string name; std::vector<TmuxWindow> windows; int active_window; };
  ```
- Update state in response to notifications
- Parse `%layout-change` layout strings: tmux encodes pane geometry as a recursive checksum,WxH,x,y format (e.g. `b]cd,200x50,0,0{100x50,0,0,3,100x50,101,0,4}`)
- Map pane geometry (cells) to pixel coordinates via cell size for rendering
- Each TmuxPane owns a ScreenBuffer + VT parser instance, fed by `%output` data

## 7. Pane / Window Manager

### 7a. Unified Pane Model
- Both standalone PTY panes and tmux panes share the same rendering/buffer infrastructure
- Pane struct wraps mode-specific source:
  ```
  struct Pane {
      int id;
      ScreenBuffer buffer;
      VtParser parser;
      enum { PTY_LOCAL, TMUX_REMOTE } source;
      union {
          struct { int master_fd; pid_t child_pid; } pty;
          struct { int tmux_pane_id; } tmux;
      };
      int pixel_x, pixel_y, pixel_w, pixel_h;   // position in window
      int cols, rows;                             // grid dimensions
      bool focused;
  };
  ```
- Input routing: keyboard events go to `write(pty.master_fd)` in PTY mode, `send-keys -t %<id>` in tmux mode

### 7b. Layout Engine
- **Standalone mode**: manual splits managed locally
  - Binary tree of splits: each node is either a leaf (pane) or a horizontal/vertical split with a ratio
  - Split operations: split-horizontal, split-vertical, close pane (rebalance tree)
  - Resize: drag dividers to adjust split ratio, propagate new cell dimensions, send SIGWINCH (PTY) or resize-pane (tmux)
- **tmux mode**: layout driven by tmux `%layout-change` notifications
  - Parse tmux layout string into the same tree structure
  - Map cell coordinates (from tmux) to pixel coordinates (cell size × position + border offsets)
  - User-initiated resize: drag dividers → send `resize-pane` to tmux → tmux responds with `%layout-change` → update local layout
  - No local layout state — tmux is authoritative, local tree is rebuilt on every `%layout-change`

### 7c. Tab Bar
- One tab per tmux window (tmux mode) or per local window group (standalone mode)
- Rendered as a strip at the top or bottom of the window (configurable)
- Display: window index + name, highlight active tab, activity/bell indicator
- Interaction: click to switch, middle-click to close, drag to reorder
- Keyboard: Ctrl+Shift+T new tab, Ctrl+Shift+W close, Ctrl+Tab / Ctrl+Shift+Tab cycle
- tmux mode: tab switch → `select-window -t <id>`, new tab → `new-window`, close → `kill-window`
- Standalone mode: tab switch changes active pane group, new tab spawns new shell

### 7d. Pane Focus & Borders
- Visual pane borders / separators between split panes (1px line, themeable color)
- Focused pane: highlighted border or distinct border color
- Focus follows click (click in pane area → focus that pane)
- Keyboard pane navigation: Ctrl+Shift+Arrow or configurable bindings
- tmux mode: focus change → `select-pane -t %<id>`

### 7e. Zoom
- Zoom active pane to fill entire window (hide other panes, hide tab bar optionally)
- Toggle via keybinding (Ctrl+Shift+Z or similar)
- tmux mode: `resize-pane -Z -t %<id>`
- Standalone mode: just adjust layout to give 100% to the zoomed pane, restore on un-zoom

## 8. Native Features (The Whole Point)

### 8a. Scrollback
- Native scroll via mouse wheel, touchpad, keyboard (Shift+PageUp/PageDown, configurable)
- Viewport offset tracked per pane, independent of tmux state
- Smooth scrolling: pixel-level offset within a line, not just line-jumping (optional, configurable)
- Auto-scroll: snap back to bottom on new output (configurable: always, only if already at bottom)
- Scroll indicator: visual marker showing position in scrollback (e.g. minimap bar, percentage, line count)
- No tmux copy-mode involved — scrollback is entirely owned by the client buffer
- In tmux mode: scrollback is populated from `%output` history since attach. For pre-attach history: optionally `capture-pane -p -S -<N>` on initial attach to seed the buffer

### 8b. Search
- Activation: Ctrl+Shift+F or configurable binding, opens search bar overlay
- Incremental: results update as you type
- Case sensitivity: insensitive by default, auto-switch to sensitive if query contains uppercase (smart case)
- Operate on logical lines (soft-wrapped lines joined) extracted from scrollback + visible content
- Boyer-Moore or two-way string matching for performance on large scrollback
- Match highlighting: all matches highlighted in buffer with distinct color, current match emphasized differently
- Navigation: Enter / Shift+Enter (or arrow keys) to jump between matches, with wraparound
- Match count display: "3 of 47 matches" in search bar
- Scope: current pane only (multi-pane search is a stretch goal)
- On close: optionally leave highlights briefly, then clear
- Integration with OSC 133 zones:
  - Keybinding to select/copy last command output (find most recent output zone, select it)
  - Search within output of last command (narrow search scope to zone)

### 8c. Selection & Clipboard
- Mouse selection:
  - Click+drag: stream selection
  - Alt+click+drag: rectangular/block selection
  - Double-click: select word (Unicode-aware word boundaries, configurable separator chars)
  - Triple-click: select entire logical line (unwrapped)
- Selection rendered as highlighted overlay, follows content if buffer scrolls
- Clipboard integration:
  - Selection auto-copied to PRIMARY (X11 middle-click paste convention)
  - Ctrl+Shift+C: copy selection to CLIPBOARD
  - Ctrl+Shift+V: paste from CLIPBOARD (wrapped in bracketed paste if pane has it enabled)
  - Middle-click: paste from PRIMARY
- OSC 52 support:
  - Write direction: application can set clipboard contents (always allowed)
  - Read direction: disabled by default (security risk), configurable opt-in
- Paste safety: warn or block if pasted content contains newlines / suspicious escape sequences (configurable)

### 8d. URL Detection & Handling
- Scan visible buffer + scrollback for URLs using a simple state machine / pattern matcher (no regex — hand-rolled recognizer for http(s)://, file://, mailto:, etc.)
- URLs rendered with underline + distinct color (themeable)
- Ctrl+click or configurable modifier+click: open URL in default browser (xdg-open)
- Hover highlight: when holding modifier key, URL under cursor gets visually emphasized
- OSC 8 hyperlinks: explicit hyperlink annotations from applications take priority over pattern detection
- OSC 8 hover: show URI in a tooltip or status bar on modifier+hover

### 8e. Notifications & Monitoring
- Bell: visual bell (flash pane briefly), optionally pass to desktop notification
- Activity monitoring: mark panes/tabs with activity indicator when they produce output while not focused
- Idle monitoring: optionally highlight panes that have had no output for a configurable duration (useful for watching builds)
- Desktop notifications via libnotify / D-Bus org.freedesktop.Notifications:
  - On bell in unfocused pane
  - On activity in monitored pane
  - Configurable: per-pane enable/disable, throttle to avoid spam

### 8f. Shell Integration (via OSC 133)
- Detect prompt boundaries emitted by shells (bash, zsh, fish)
- Keybindings:
  - Jump to previous/next prompt in scrollback (Ctrl+Shift+Up/Down)
  - Select output of last command (Ctrl+Shift+A or similar)
  - Copy output of last command to clipboard
- Visual: subtle separator or marker at prompt boundaries in scrollback (optional, themeable)

## 9. Defaults & Theming

Philosophy: ship good defaults, minimize configuration surface. No config file parser at startup — if anything needs to be tunable, compile-time constants or a minimal key=value file later.

### 9a. Colors
- True color (24-bit) throughout — COLORTERM=truecolor, TERM=xterm-256color (or custom terminfo)
- Default color scheme: ship one good dark theme baked in (e.g. a tasteful base16 variant)
- 256-color palette: standard xterm-256color mapping, not configurable
- Minimum contrast enforcement: if fg/bg are too similar, nudge fg (accessibility baseline)

### 9b. Font
- Default: query fontconfig for system default monospace font, sensible size (11pt or 12pt)
- Fallback chain: system monospace → Noto Sans Mono → Noto Color Emoji (auto-discovered via fontconfig)
- Font size adjustable at runtime: Ctrl+Plus / Ctrl+Minus / Ctrl+0 (reset)
- No font configuration beyond system fontconfig — if user wants a different font, they set their system monospace preference

### 9c. Terminal Identity
- TERM: `xterm-256color` (widely compatible) — or ship a custom terminfo entry that advertises true color, kitty keyboard, etc.
- COLORTERM: `truecolor`
- TERM_PROGRAM: `rivt`
- TERM_PROGRAM_VERSION: version string

### 9d. Key Bindings
- Hardcoded, sane defaults:
  - Ctrl+Shift+C / Ctrl+Shift+V — copy/paste clipboard
  - Ctrl+Shift+F — search
  - Ctrl+Shift+T — new tab
  - Ctrl+Shift+W — close tab/pane
  - Ctrl+Shift+N — new window
  - Ctrl+Plus / Ctrl+Minus / Ctrl+0 — font size
  - Ctrl+Shift+Up/Down — jump to prev/next prompt (OSC 133)
  - Ctrl+Shift+Arrow — navigate panes
  - Ctrl+Tab / Ctrl+Shift+Tab — cycle tabs
  - Shift+PageUp / Shift+PageDown — scroll
  - Ctrl+Shift+Z — zoom pane
- No keybinding customization initially — just pick good ones and commit

### 9e. Behavior Defaults
- Scrollback: 10,000 lines
- Auto-scroll: snap to bottom on new output only if viewport was already at bottom
- Visual bell, no audible bell
- Bracketed paste: always wrap if pane has it enabled
- Paste safety: warn on multi-line paste containing control characters
- OSC 52 write: allowed, OSC 52 read: denied
- Focus reporting: enabled (DECSET 1004)
- URL detection: always on, Ctrl+click to open
- Subpixel rendering: auto-detect via fontconfig

## 10. Standalone PTY Mode

- Default mode when launched without `--tmux` flag
- `forkpty()` to create PTY + fork child process
- Child: exec user's shell ($SHELL, fallback to /bin/sh)
- Environment setup: TERM, COLORTERM, TERM_PROGRAM, TERM_PROGRAM_VERSION, SHELL, plus inherit parent env
- PTY master fd added to epoll loop alongside platform event fd
- SIGCHLD handling: detect child exit, display "[process exited]" or close pane
- SIGWINCH propagation: on pane resize, `ioctl(master_fd, TIOCSWINSZ, &ws)` to inform child
- Shares all infrastructure with tmux mode: same VT parser, ScreenBuffer, renderer, selection, search, scrollback
- Local pane/tab management via the layout tree (section 7b)
- New tab/split spawns a new PTY + shell

## 11. Stretch Goals

- GPU-accelerated rendering via Vulkan or wgpu
- Sixel / kitty image protocol support
- Font ligature support (discretionary ligatures in coding fonts)
- Session picker UI (list tmux sessions, attach/detach)
- Remote SSH bootstrapping (ssh + tmux -CC in one step, like iTerm2)
- Multiple windows (separate OS windows, each with own tab/pane tree)
- Background transparency / blur (compositor-dependent)
- Synchronized rendering (DCS = 1 s / DCS = 2 s) to reduce flicker during rapid updates

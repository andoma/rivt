#pragma once
#include "terminal/screen_buffer.h"
#include "terminal/vt_parser.h"
#include "pty/pty.h"
#include "core/event_loop.h"
#include "core/config.h"
#include "core/util.h"
#include <functional>
#include <memory>
#include <string>

namespace rivt {

class Platform;

struct PaneRect {
    int x = 0, y = 0, w = 0, h = 0;
};

class Pane {
public:
    Pane(int cols, int rows, const Config &config);
    ~Pane();

    // Spawn shell and register PTY fd with event loop
    bool spawn_shell(EventLoop &loop);

    // Remove PTY fd from event loop (called before destruction)
    void detach(EventLoop &loop);

    // Write data to the PTY (or write_callback if set)
    void write(const std::string &data);
    void write(const char *data, size_t len);

    // Feed data directly into parser (for tmux %output)
    void feed_data(const char *buf, size_t len);

    // Resize terminal grid
    void resize(int cols, int rows);

    // Check if child shell is alive
    bool alive() const { if (m_write_callback) return true; return m_pty.alive(); }

    // When set, write() routes here instead of to PTY
    std::function<void(const std::string &)> m_write_callback;

    // Accessors
    ScreenBuffer &screen() { return m_screen; }
    const ScreenBuffer &screen() const { return m_screen; }
    VtParser &parser() { return m_parser; }
    Pty &pty() { return m_pty; }

    // Selection state (owned by pane, not screen buffer)
    bool selecting = false;
    int click_count = 0;
    uint64_t last_click_ms = 0;
    int last_click_col = -1, last_click_row = -1;

    // Layout rect (pixel coordinates within the window)
    PaneRect rect;

    // Set up platform-dependent callbacks (title, clipboard, etc.)
    void setup_callbacks(Platform *platform, const Config &config);

    // Callback: request re-render
    std::function<void()> on_needs_render;

    // Callback: PTY died
    std::function<void(Pane *)> on_dead;

    // Callback: title changed
    std::function<void(Pane *, const std::string &)> on_title_change;

    // Activity flag (set when output received while not focused)
    bool has_activity = false;

    // Callback: tmux control mode detected in PTY output (\033P1000p)
    std::function<void(Pane *)> on_tmux_control_mode;

    // When set, raw PTY reads go here instead of VtParser (used for tmux PTY mode)
    std::function<void(const char *, int)> m_pty_data_override;

private:
    ScreenBuffer m_screen;
    VtParser m_parser;
    Pty m_pty;
    int m_pty_fd_registered = -1;
};

} // namespace rivt

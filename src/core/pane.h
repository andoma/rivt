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

    // Write data to the PTY
    void write(const std::string &data);
    void write(const char *data, size_t len);

    // Resize terminal grid
    void resize(int cols, int rows);

    // Check if child shell is alive
    bool alive() const { return pty_.alive(); }

    // Accessors
    ScreenBuffer &screen() { return screen_; }
    const ScreenBuffer &screen() const { return screen_; }
    VtParser &parser() { return parser_; }
    Pty &pty() { return pty_; }

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

private:
    ScreenBuffer screen_;
    VtParser parser_;
    Pty pty_;
    int pty_fd_registered_ = -1;
};

} // namespace rivt

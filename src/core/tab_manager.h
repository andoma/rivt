#pragma once
#include "core/tab.h"
#include "core/config.h"
#include "core/event_loop.h"
#include <functional>
#include <memory>
#include <vector>

namespace rivt {

class Platform;

class TabManager {
public:
    TabManager(Config &config, EventLoop &loop, Platform *platform);

    // Create a new tab with a single pane, make it active. Returns the new tab.
    Tab *new_tab();

    // Close tab by index. Returns false if it was the last tab (caller should quit).
    bool close_tab(int index);

    // Activate tab by index
    void activate_tab(int index);

    // Cycle to next/previous tab
    void next_tab();
    void prev_tab();

    // Active tab
    Tab *active_tab() { return active_index_ >= 0 ? tabs_[active_index_].get() : nullptr; }
    const Tab *active_tab() const { return active_index_ >= 0 ? tabs_[active_index_].get() : nullptr; }
    int active_index() const { return active_index_; }

    // Currently focused pane (in active tab)
    Pane *focused_pane();

    // Split the focused pane in the active tab
    Pane *split_pane(SplitDir dir);

    // Close the focused pane in the active tab.
    // Returns false if the last pane in the last tab was closed (caller should quit).
    bool close_focused_pane();

    // Navigate pane focus in the active tab
    void navigate_pane(NavDir dir);

    // Set cell dimensions (from renderer metrics)
    void set_cell_size(int cell_w, int cell_h) { cell_w_ = cell_w; cell_h_ = cell_h; }

    // Recompute layout for all panes in active tab
    void recompute_layout(int content_x, int content_y, int content_w, int content_h);

    // Tab count
    int tab_count() const { return (int)tabs_.size(); }

    // Access tabs
    const std::vector<std::unique_ptr<Tab>> &tabs() const { return tabs_; }

    // Check all panes across all tabs for dead shells, remove them.
    // Returns false if all tabs are gone (caller should quit).
    bool reap_dead_panes();

    // Callback: request re-render
    std::function<void()> on_needs_render;

    // Callback: should quit
    std::function<void()> on_quit;

private:
    Pane *create_pane();
    void setup_pane(Pane *pane);

    Config &config_;
    EventLoop &loop_;
    Platform *platform_;

    std::vector<std::unique_ptr<Tab>> tabs_;
    int active_index_ = -1;
    int next_tab_id_ = 1;

    // Layout params (cached from last recompute)
    int content_x_ = 0, content_y_ = 0, content_w_ = 0, content_h_ = 0;
    int cell_w_ = 0, cell_h_ = 0;
};

} // namespace rivt

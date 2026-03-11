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

    // Close tab by pointer. Returns false if it was the last tab.
    bool close_tab_ptr(Tab *tab);

    // Create an empty tab (no shell spawned) for tmux use
    Tab *new_empty_tab(const std::string &title);

    // Add a pane to a tab without spawning a shell (for tmux use)
    Pane *add_pane_to_tab(Tab *tab, int cols, int rows);

    // Remove a specific pane from a tab
    bool remove_pane(Tab *tab, Pane *pane);

    // Activate tab by index
    void activate_tab(int index);

    // Cycle to next/previous tab
    void next_tab();
    void prev_tab();

    // Active tab
    Tab *active_tab() { return m_active_index >= 0 ? m_tabs[m_active_index].get() : nullptr; }
    const Tab *active_tab() const { return m_active_index >= 0 ? m_tabs[m_active_index].get() : nullptr; }
    int active_index() const { return m_active_index; }

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
    void set_cell_size(int cell_w, int cell_h) { m_cell_w = cell_w; m_cell_h = cell_h; }

    // Recompute layout for all panes in active tab
    void recompute_layout(int content_x, int content_y, int content_w, int content_h);

    // Tab count
    int tab_count() const { return (int)m_tabs.size(); }

    // Access tabs
    const std::vector<std::unique_ptr<Tab>> &tabs() const { return m_tabs; }

    // Check all panes across all tabs for dead shells, remove them.
    // Returns false if all tabs are gone (caller should quit).
    bool reap_dead_panes();

    // Callback: request re-render
    std::function<void()> on_needs_render;

    // Callback: should quit
    std::function<void()> on_quit;

    // Callback: tmux control mode detected in a pane
    std::function<void(Pane *)> on_tmux_control_mode;

private:
    Pane *create_pane();
    void setup_pane(Pane *pane);

    Config &m_config;
    EventLoop &m_loop;
    Platform *m_platform;

    std::vector<std::unique_ptr<Tab>> m_tabs;
    int m_active_index = -1;
    int m_next_tab_id = 1;

    // Layout params (cached from last recompute)
    int m_content_x = 0, m_content_y = 0, m_content_w = 0, m_content_h = 0;
    int m_cell_w = 0, m_cell_h = 0;
};

} // namespace rivt

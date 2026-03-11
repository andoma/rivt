#include "core/tab_manager.h"
#include "platform/platform.h"
#include "render/font.h"

namespace rivt {

TabManager::TabManager(Config &config, EventLoop &loop, Platform *platform)
    : m_config(config), m_loop(loop), m_platform(platform)
{
}

Pane *TabManager::create_pane() {
    // Compute initial grid size from content area
    // We'll use 80x24 as default if no content area set yet
    int cols = 80, rows = 24;
    if (m_content_w > 0 && m_content_h > 0) {
        // These will be corrected by compute_layout later
        cols = 80;
        rows = 24;
    }
    auto pane = std::make_unique<Pane>(cols, rows, m_config);
    Pane *raw = pane.get();

    setup_pane(raw);
    pane->setup_callbacks(m_platform, m_config);

    if (!pane->spawn_shell(m_loop)) {
        return nullptr;
    }

    // Add to active tab
    active_tab()->panes.push_back(std::move(pane));
    return raw;
}

void TabManager::setup_pane(Pane *pane) {
    pane->on_needs_render = [this, pane]() {
        // If pane is not in the active tab, mark activity
        Tab *at = active_tab();
        if (at && at->focused_pane != pane) {
            pane->has_activity = true;
        }
        // Check if pane is in a non-active tab
        for (int i = 0; i < (int)m_tabs.size(); i++) {
            if (i != m_active_index) {
                for (auto &p : m_tabs[i]->panes) {
                    if (p.get() == pane) {
                        m_tabs[i]->has_activity = true;
                        break;
                    }
                }
            }
        }
        if (on_needs_render) on_needs_render();
    };

    pane->on_dead = [this](Pane *) {
        // Will be cleaned up by reap_dead_panes
        if (on_needs_render) on_needs_render();
    };

    pane->on_title_change = [this](Pane *p, const std::string &title) {
        // Update the tab title for whichever tab owns this pane
        for (auto &tab : m_tabs) {
            if (tab->focused_pane == p) {
                tab->title = title;
                if (on_needs_render) on_needs_render();
                break;
            }
        }
    };

    pane->on_tmux_control_mode = [this](Pane *p) {
        if (on_tmux_control_mode) on_tmux_control_mode(p);
    };
}

Tab *TabManager::new_tab() {
    auto tab = std::make_unique<Tab>();
    tab->id = m_next_tab_id++;
    tab->title = "Terminal";

    Tab *raw = tab.get();
    m_tabs.push_back(std::move(tab));
    m_active_index = (int)m_tabs.size() - 1;

    // Create initial pane
    // We need a temporary grid size; compute_layout will fix it
    auto pane = std::make_unique<Pane>(80, 24, m_config);
    Pane *pane_raw = pane.get();

    setup_pane(pane_raw);
    pane_raw->setup_callbacks(m_platform, m_config);

    if (!pane_raw->spawn_shell(m_loop)) {
        m_tabs.pop_back();
        m_active_index = m_tabs.empty() ? -1 : (int)m_tabs.size() - 1;
        return nullptr;
    }

    raw->panes.push_back(std::move(pane));
    raw->focused_pane = pane_raw;
    raw->layout.init(pane_raw);

    // Recompute layout if we have dimensions
    if (m_content_w > 0 && m_content_h > 0) {
        recompute_layout(m_content_x, m_content_y, m_content_w, m_content_h);
    }

    return raw;
}

bool TabManager::close_tab(int index) {
    if (index < 0 || index >= (int)m_tabs.size()) return true;

    // Detach all panes from event loop
    for (auto &pane : m_tabs[index]->panes) {
        pane->detach(m_loop);
    }

    m_tabs.erase(m_tabs.begin() + index);

    if (m_tabs.empty()) {
        m_active_index = -1;
        return false;
    }

    if (m_active_index >= (int)m_tabs.size())
        m_active_index = (int)m_tabs.size() - 1;

    return true;
}

bool TabManager::close_tab_ptr(Tab *tab) {
    for (int i = 0; i < (int)m_tabs.size(); i++) {
        if (m_tabs[i].get() == tab) {
            return close_tab(i);
        }
    }
    return true;
}

Tab *TabManager::new_empty_tab(const std::string &title) {
    auto tab = std::make_unique<Tab>();
    tab->id = m_next_tab_id++;
    tab->title = title;

    Tab *raw = tab.get();
    m_tabs.push_back(std::move(tab));
    m_active_index = (int)m_tabs.size() - 1;

    return raw;
}

Pane *TabManager::add_pane_to_tab(Tab *tab, int cols, int rows) {
    auto pane = std::make_unique<Pane>(cols, rows, m_config);
    Pane *raw = pane.get();

    setup_pane(raw);
    pane->setup_callbacks(m_platform, m_config);

    tab->panes.push_back(std::move(pane));
    if (!tab->focused_pane) tab->focused_pane = raw;

    return raw;
}

bool TabManager::remove_pane(Tab *tab, Pane *pane) {
    pane->detach(m_loop);

    // Update focus if needed
    if (tab->focused_pane == pane) {
        tab->focused_pane = nullptr;
        for (auto &p : tab->panes) {
            if (p.get() != pane) {
                tab->focused_pane = p.get();
                break;
            }
        }
    }

    auto it = std::find_if(tab->panes.begin(), tab->panes.end(),
        [pane](const std::unique_ptr<Pane> &p) { return p.get() == pane; });
    if (it != tab->panes.end()) {
        tab->panes.erase(it);
        return true;
    }
    return false;
}

void TabManager::activate_tab(int index) {
    if (index < 0 || index >= (int)m_tabs.size()) return;
    m_active_index = index;
    m_tabs[index]->has_activity = false;
    // Update title from focused pane
    if (on_needs_render) on_needs_render();
}

void TabManager::next_tab() {
    if (m_tabs.size() <= 1) return;
    activate_tab((m_active_index + 1) % (int)m_tabs.size());
}

void TabManager::prev_tab() {
    if (m_tabs.size() <= 1) return;
    activate_tab((m_active_index + (int)m_tabs.size() - 1) % (int)m_tabs.size());
}

Pane *TabManager::focused_pane() {
    Tab *tab = active_tab();
    return tab ? tab->focused_pane : nullptr;
}

Pane *TabManager::split_pane(SplitDir dir) {
    Tab *tab = active_tab();
    if (!tab || !tab->focused_pane) return nullptr;

    Pane *new_pane = create_pane();
    if (!new_pane) return nullptr;

    if (!tab->layout.split(tab->focused_pane, new_pane, dir)) {
        // Split failed; remove the pane we just created
        new_pane->detach(m_loop);
        tab->panes.pop_back();
        return nullptr;
    }

    tab->focused_pane = new_pane;

    // Recompute layout
    if (m_content_w > 0 && m_content_h > 0) {
        recompute_layout(m_content_x, m_content_y, m_content_w, m_content_h);
    }

    return new_pane;
}

bool TabManager::close_focused_pane() {
    Tab *tab = active_tab();
    if (!tab || !tab->focused_pane) return true;

    Pane *target = tab->focused_pane;

    // If this is the only pane in the only tab, we should quit
    if (tab->panes.size() == 1 && m_tabs.size() == 1) {
        return false;
    }

    // If this is the only pane in this tab, close the tab
    if (tab->panes.size() == 1) {
        return close_tab(m_active_index);
    }

    // Remove from layout
    tab->layout.remove(target);

    // Find a new pane to focus
    std::vector<Pane *> remaining;
    tab->layout.collect_panes(remaining);
    tab->focused_pane = remaining.empty() ? nullptr : remaining[0];

    // Detach and remove
    target->detach(m_loop);
    auto it = std::find_if(tab->panes.begin(), tab->panes.end(),
        [target](const std::unique_ptr<Pane> &p) { return p.get() == target; });
    if (it != tab->panes.end()) {
        tab->panes.erase(it);
    }

    // Recompute layout
    if (m_content_w > 0 && m_content_h > 0) {
        recompute_layout(m_content_x, m_content_y, m_content_w, m_content_h);
    }

    return true;
}

void TabManager::navigate_pane(NavDir dir) {
    Tab *tab = active_tab();
    if (!tab || !tab->focused_pane) return;

    Pane *target = tab->layout.navigate(tab->focused_pane, dir);
    if (target && target != tab->focused_pane) {
        tab->focused_pane = target;
        target->has_activity = false;
        if (on_needs_render) on_needs_render();
    }
}

void TabManager::recompute_layout(int content_x, int content_y, int content_w, int content_h) {
    m_content_x = content_x;
    m_content_y = content_y;
    m_content_w = content_w;
    m_content_h = content_h;

    Tab *tab = active_tab();
    if (!tab) return;

    // Skip layout computation for tmux-managed tabs (pane rects set by TmuxController)
    if (tab->tmux_managed) return;

    tab->layout.compute_layout(content_x, content_y, content_w, content_h,
                                2, m_cell_w, m_cell_h);
}

bool TabManager::reap_dead_panes() {
    bool any_removed = false;

    for (int t = (int)m_tabs.size() - 1; t >= 0; t--) {
        Tab &tab = *m_tabs[t];
        for (int p = (int)tab.panes.size() - 1; p >= 0; p--) {
            if (!tab.panes[p]->alive()) {
                Pane *dead = tab.panes[p].get();

                if (tab.panes.size() == 1) {
                    // Last pane in tab — close the tab
                    dead->detach(m_loop);
                    m_tabs.erase(m_tabs.begin() + t);
                    if (m_active_index >= (int)m_tabs.size())
                        m_active_index = (int)m_tabs.size() - 1;
                    any_removed = true;
                    break;
                }

                // Remove from layout
                tab.layout.remove(dead);

                // Update focus
                if (tab.focused_pane == dead) {
                    std::vector<Pane *> remaining;
                    tab.layout.collect_panes(remaining);
                    tab.focused_pane = remaining.empty() ? nullptr : remaining[0];
                }

                dead->detach(m_loop);
                tab.panes.erase(tab.panes.begin() + p);
                any_removed = true;
            }
        }
    }

    if (any_removed && m_content_w > 0 && m_content_h > 0) {
        Tab *tab = active_tab();
        if (tab) {
            tab->layout.compute_layout(m_content_x, m_content_y, m_content_w, m_content_h,
                                        2, m_cell_w, m_cell_h);
        }
    }

    return !m_tabs.empty();
}

} // namespace rivt

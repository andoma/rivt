#include "core/tab_manager.h"
#include "platform/platform.h"
#include "render/font.h"

namespace rivt {

TabManager::TabManager(Config &config, EventLoop &loop, Platform *platform)
    : config_(config), loop_(loop), platform_(platform)
{
}

Pane *TabManager::create_pane() {
    // Compute initial grid size from content area
    // We'll use 80x24 as default if no content area set yet
    int cols = 80, rows = 24;
    if (content_w_ > 0 && content_h_ > 0) {
        // These will be corrected by compute_layout later
        cols = 80;
        rows = 24;
    }
    auto pane = std::make_unique<Pane>(cols, rows, config_);
    Pane *raw = pane.get();

    setup_pane(raw);
    pane->setup_callbacks(platform_, config_);

    if (!pane->spawn_shell(loop_)) {
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
        for (int i = 0; i < (int)tabs_.size(); i++) {
            if (i != active_index_) {
                for (auto &p : tabs_[i]->panes) {
                    if (p.get() == pane) {
                        tabs_[i]->has_activity = true;
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
        for (auto &tab : tabs_) {
            if (tab->focused_pane == p) {
                tab->title = title;
                if (on_needs_render) on_needs_render();
                break;
            }
        }
    };
}

Tab *TabManager::new_tab() {
    auto tab = std::make_unique<Tab>();
    tab->id = next_tab_id_++;
    tab->title = "Terminal";

    Tab *raw = tab.get();
    tabs_.push_back(std::move(tab));
    active_index_ = (int)tabs_.size() - 1;

    // Create initial pane
    // We need a temporary grid size; compute_layout will fix it
    auto pane = std::make_unique<Pane>(80, 24, config_);
    Pane *pane_raw = pane.get();

    setup_pane(pane_raw);
    pane_raw->setup_callbacks(platform_, config_);

    if (!pane_raw->spawn_shell(loop_)) {
        tabs_.pop_back();
        active_index_ = tabs_.empty() ? -1 : (int)tabs_.size() - 1;
        return nullptr;
    }

    raw->panes.push_back(std::move(pane));
    raw->focused_pane = pane_raw;
    raw->layout.init(pane_raw);

    // Recompute layout if we have dimensions
    if (content_w_ > 0 && content_h_ > 0) {
        recompute_layout(content_x_, content_y_, content_w_, content_h_);
    }

    return raw;
}

bool TabManager::close_tab(int index) {
    if (index < 0 || index >= (int)tabs_.size()) return true;

    // Detach all panes from event loop
    for (auto &pane : tabs_[index]->panes) {
        pane->detach(loop_);
    }

    tabs_.erase(tabs_.begin() + index);

    if (tabs_.empty()) {
        active_index_ = -1;
        return false;
    }

    if (active_index_ >= (int)tabs_.size())
        active_index_ = (int)tabs_.size() - 1;

    return true;
}

void TabManager::activate_tab(int index) {
    if (index < 0 || index >= (int)tabs_.size()) return;
    active_index_ = index;
    tabs_[index]->has_activity = false;
    // Update title from focused pane
    if (on_needs_render) on_needs_render();
}

void TabManager::next_tab() {
    if (tabs_.size() <= 1) return;
    activate_tab((active_index_ + 1) % (int)tabs_.size());
}

void TabManager::prev_tab() {
    if (tabs_.size() <= 1) return;
    activate_tab((active_index_ + (int)tabs_.size() - 1) % (int)tabs_.size());
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
        new_pane->detach(loop_);
        tab->panes.pop_back();
        return nullptr;
    }

    tab->focused_pane = new_pane;

    // Recompute layout
    if (content_w_ > 0 && content_h_ > 0) {
        recompute_layout(content_x_, content_y_, content_w_, content_h_);
    }

    return new_pane;
}

bool TabManager::close_focused_pane() {
    Tab *tab = active_tab();
    if (!tab || !tab->focused_pane) return true;

    Pane *target = tab->focused_pane;

    // If this is the only pane in the only tab, we should quit
    if (tab->panes.size() == 1 && tabs_.size() == 1) {
        return false;
    }

    // If this is the only pane in this tab, close the tab
    if (tab->panes.size() == 1) {
        return close_tab(active_index_);
    }

    // Remove from layout
    tab->layout.remove(target);

    // Find a new pane to focus
    std::vector<Pane *> remaining;
    tab->layout.collect_panes(remaining);
    tab->focused_pane = remaining.empty() ? nullptr : remaining[0];

    // Detach and remove
    target->detach(loop_);
    auto it = std::find_if(tab->panes.begin(), tab->panes.end(),
        [target](const std::unique_ptr<Pane> &p) { return p.get() == target; });
    if (it != tab->panes.end()) {
        tab->panes.erase(it);
    }

    // Recompute layout
    if (content_w_ > 0 && content_h_ > 0) {
        recompute_layout(content_x_, content_y_, content_w_, content_h_);
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
    content_x_ = content_x;
    content_y_ = content_y;
    content_w_ = content_w;
    content_h_ = content_h;

    Tab *tab = active_tab();
    if (!tab) return;

    // Get cell dimensions from the first pane's screen metrics are not directly
    // available here. We pass 0 for cell_w/cell_h and let Pane::resize handle grid calc.
    // Actually, we need cell dimensions for snapping. We'll get them from the renderer
    // through the caller. For now, use border_width=2.
    // The caller should pass cell_w/cell_h via a different mechanism or we store them.
    // For simplicity, store cell dimensions.
    tab->layout.compute_layout(content_x, content_y, content_w, content_h,
                                2, cell_w_, cell_h_);
}

bool TabManager::reap_dead_panes() {
    bool any_removed = false;

    for (int t = (int)tabs_.size() - 1; t >= 0; t--) {
        Tab &tab = *tabs_[t];
        for (int p = (int)tab.panes.size() - 1; p >= 0; p--) {
            if (!tab.panes[p]->alive()) {
                Pane *dead = tab.panes[p].get();

                if (tab.panes.size() == 1) {
                    // Last pane in tab — close the tab
                    dead->detach(loop_);
                    tabs_.erase(tabs_.begin() + t);
                    if (active_index_ >= (int)tabs_.size())
                        active_index_ = (int)tabs_.size() - 1;
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

                dead->detach(loop_);
                tab.panes.erase(tab.panes.begin() + p);
                any_removed = true;
            }
        }
    }

    if (any_removed && content_w_ > 0 && content_h_ > 0) {
        Tab *tab = active_tab();
        if (tab) {
            tab->layout.compute_layout(content_x_, content_y_, content_w_, content_h_,
                                        2, cell_w_, cell_h_);
        }
    }

    return !tabs_.empty();
}

} // namespace rivt

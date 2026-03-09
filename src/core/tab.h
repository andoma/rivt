#pragma once
#include "core/layout.h"
#include "core/pane.h"
#include <memory>
#include <string>
#include <vector>

namespace rivt {

struct Tab {
    int id;
    std::string title;
    LayoutTree layout;
    std::vector<std::unique_ptr<Pane>> panes;
    Pane *focused_pane = nullptr;
    bool has_activity = false;
    bool tmux_managed = false;

    // Find and remove a pane by pointer. Returns true if found.
    bool remove_pane(Pane *pane);
};

} // namespace rivt

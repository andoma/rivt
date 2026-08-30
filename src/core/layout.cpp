#include "core/layout.h"
#include <algorithm>
#include <cmath>
#include <climits>

namespace rivt {

LayoutTree::LayoutTree() = default;

void LayoutTree::init(Pane *pane) {
    m_root = std::make_unique<LayoutNode>();
    m_root->pane = pane;
}

bool LayoutTree::split(Pane *target, Pane *new_pane, SplitDir dir) {
    if (!m_root) return false;

    // Find the leaf containing target
    LayoutNode *leaf = find_leaf(m_root.get(), target);
    if (!leaf) return false;

    // Convert this leaf into an internal node with two children
    auto first_child = std::make_unique<LayoutNode>();
    first_child->pane = target;

    auto second_child = std::make_unique<LayoutNode>();
    second_child->pane = new_pane;

    leaf->pane = nullptr;
    leaf->split_dir = dir;
    leaf->ratio = 0.5f;
    leaf->first = std::move(first_child);
    leaf->second = std::move(second_child);

    return true;
}

bool LayoutTree::remove(Pane *target) {
    if (!m_root) return false;

    // Special case: root is the target leaf
    if (m_root->is_leaf() && m_root->pane == target) {
        m_root.reset();
        return true;
    }

    // Find parent of the leaf containing target
    LayoutNode *parent = find_parent(m_root.get(), target);
    if (!parent) return false;

    // Determine which child contains target and which is the sibling
    std::unique_ptr<LayoutNode> sibling;
    if (parent->first && parent->first->is_leaf() && parent->first->pane == target) {
        sibling = std::move(parent->second);
    } else if (parent->second && parent->second->is_leaf() && parent->second->pane == target) {
        sibling = std::move(parent->first);
    } else {
        return false;
    }

    // Replace parent with sibling's content
    parent->pane = sibling->pane;
    parent->split_dir = sibling->split_dir;
    parent->ratio = sibling->ratio;
    parent->first = std::move(sibling->first);
    parent->second = std::move(sibling->second);

    return true;
}

void LayoutTree::compute_layout(int x, int y, int w, int h, int border_width,
                                 int cell_w, int cell_h) {
    if (m_root)
        compute_node(m_root.get(), x, y, w, h, border_width, cell_w, cell_h);
}

void LayoutTree::compute_node(LayoutNode *node, int x, int y, int w, int h,
                               int border_width, int cell_w, int cell_h) {
    node->x = x;
    node->y = y;
    node->w = w;
    node->h = h;
    if (node->is_leaf()) {
        node->pane->rect = {x, y, w, h};
        // Compute grid dimensions from pixel size
        int cols = cell_w > 0 ? w / cell_w : 1;
        int rows = cell_h > 0 ? h / cell_h : 1;
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        if (cols != node->pane->screen().cols() || rows != node->pane->screen().rows()) {
            node->pane->resize(cols, rows);
        }
        return;
    }

    if (node->split_dir == SplitDir::Vertical) {
        // Split left/right
        int first_w = (int)(w * node->ratio) - border_width / 2;
        int second_x = x + first_w + border_width;
        int second_w = w - first_w - border_width;
        // Snap to cell boundaries
        if (cell_w > 0) {
            first_w = (first_w / cell_w) * cell_w;
            second_x = x + first_w + border_width;
            second_w = w - first_w - border_width;
        }
        compute_node(node->first.get(), x, y, first_w, h, border_width, cell_w, cell_h);
        compute_node(node->second.get(), second_x, y, second_w, h, border_width, cell_w, cell_h);
    } else {
        // Split top/bottom
        int first_h = (int)(h * node->ratio) - border_width / 2;
        int second_y = y + first_h + border_width;
        int second_h = h - first_h - border_width;
        if (cell_h > 0) {
            first_h = (first_h / cell_h) * cell_h;
            second_y = y + first_h + border_width;
            second_h = h - first_h - border_width;
        }
        compute_node(node->first.get(), x, y, w, first_h, border_width, cell_w, cell_h);
        compute_node(node->second.get(), x, second_y, w, second_h, border_width, cell_w, cell_h);
    }
}

static LayoutNode *divider_at_node(LayoutNode *node, int px, int py, int slop) {
    if (!node || node->is_leaf()) return nullptr;
    // Deepest match wins: check children before this node's own divider.
    if (LayoutNode *d = divider_at_node(node->first.get(), px, py, slop)) return d;
    if (LayoutNode *d = divider_at_node(node->second.get(), px, py, slop)) return d;
    if (node->split_dir == SplitDir::Vertical) {
        // Divider is the vertical band between first's right edge and
        // second's left edge.
        int lo = node->first->x + node->first->w - slop;
        int hi = node->second->x + slop;
        if (px >= lo && px <= hi && py >= node->y && py < node->y + node->h)
            return node;
    } else {
        int lo = node->first->y + node->first->h - slop;
        int hi = node->second->y + slop;
        if (py >= lo && py <= hi && px >= node->x && px < node->x + node->w)
            return node;
    }
    return nullptr;
}

LayoutNode *LayoutTree::divider_at(int px, int py, int slop) {
    return divider_at_node(m_root.get(), px, py, slop);
}

void LayoutTree::drag_divider(LayoutNode *split, int px, int py) {
    if (!split || split->is_leaf()) return;
    float r;
    if (split->split_dir == SplitDir::Vertical)
        r = split->w > 0 ? (float)(px - split->x) / split->w : split->ratio;
    else
        r = split->h > 0 ? (float)(py - split->y) / split->h : split->ratio;
    if (r < 0.05f) r = 0.05f;
    if (r > 0.95f) r = 0.95f;
    split->ratio = r;
}

static bool subtree_contains(const LayoutNode *node, Pane *t) {
    if (!node) return false;
    if (node->is_leaf()) return node->pane == t;
    return subtree_contains(node->first.get(), t) ||
           subtree_contains(node->second.get(), t);
}

static LayoutNode *edge_split_node(LayoutNode *node, Pane *t, SplitDir dir) {
    if (!node || node->is_leaf()) return nullptr;
    if (LayoutNode *d = edge_split_node(node->first.get(), t, dir)) return d;
    if (LayoutNode *d = edge_split_node(node->second.get(), t, dir)) return d;
    if (node->split_dir == dir && subtree_contains(node->first.get(), t))
        return node;
    return nullptr;
}

bool LayoutTree::resize_edge(Pane *target, bool horizontal, int delta) {
    SplitDir dir = horizontal ? SplitDir::Vertical : SplitDir::Horizontal;
    LayoutNode *split = edge_split_node(m_root.get(), target, dir);
    if (!split) return false;
    int extent = horizontal ? split->w : split->h;
    if (extent <= 0) return false;
    float r = split->ratio + (float)delta / (float)extent;
    if (r < 0.05f) r = 0.05f;
    if (r > 0.95f) r = 0.95f;
    split->ratio = r;
    return true;
}

Pane *LayoutTree::pane_at(int px, int py) const {
    if (!m_root) return nullptr;
    return pane_at_node(m_root.get(), px, py);
}

Pane *LayoutTree::pane_at_node(const LayoutNode *node, int px, int py) const {
    if (node->is_leaf()) {
        const auto &r = node->pane->rect;
        if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h)
            return node->pane;
        return nullptr;
    }

    Pane *p = pane_at_node(node->first.get(), px, py);
    if (p) return p;
    return pane_at_node(node->second.get(), px, py);
}

Pane *LayoutTree::navigate(Pane *from, NavDir dir) const {
    if (!m_root) return from;

    std::vector<PaneCenter> centers;
    collect_centers(m_root.get(), centers);

    // Find center of 'from'
    int from_cx = 0, from_cy = 0;
    for (auto &pc : centers) {
        if (pc.pane == from) {
            from_cx = pc.cx;
            from_cy = pc.cy;
            break;
        }
    }

    Pane *best = from;
    int best_dist = INT_MAX;

    for (auto &pc : centers) {
        if (pc.pane == from) continue;

        int dx = pc.cx - from_cx;
        int dy = pc.cy - from_cy;

        bool valid = false;
        switch (dir) {
            case NavDir::Left:  valid = dx < 0; break;
            case NavDir::Right: valid = dx > 0; break;
            case NavDir::Up:    valid = dy < 0; break;
            case NavDir::Down:  valid = dy > 0; break;
        }

        if (!valid) continue;

        int dist = dx * dx + dy * dy;
        if (dist < best_dist) {
            best_dist = dist;
            best = pc.pane;
        }
    }

    return best;
}

void LayoutTree::collect_panes(std::vector<Pane *> &out) const {
    if (m_root) collect_node(m_root.get(), out);
}

void LayoutTree::collect_node(const LayoutNode *node, std::vector<Pane *> &out) const {
    if (node->is_leaf()) {
        out.push_back(node->pane);
    } else {
        collect_node(node->first.get(), out);
        collect_node(node->second.get(), out);
    }
}

void LayoutTree::collect_centers(const LayoutNode *node, std::vector<PaneCenter> &out) const {
    if (node->is_leaf()) {
        const auto &r = node->pane->rect;
        out.push_back({node->pane, r.x + r.w / 2, r.y + r.h / 2});
    } else {
        collect_centers(node->first.get(), out);
        collect_centers(node->second.get(), out);
    }
}

LayoutNode *LayoutTree::find_leaf(LayoutNode *node, Pane *target) const {
    if (node->is_leaf()) {
        return node->pane == target ? node : nullptr;
    }
    LayoutNode *found = find_leaf(node->first.get(), target);
    if (found) return found;
    return find_leaf(node->second.get(), target);
}

LayoutNode *LayoutTree::find_parent(LayoutNode *node, Pane *target) const {
    if (node->is_leaf()) return nullptr;

    if ((node->first && node->first->is_leaf() && node->first->pane == target) ||
        (node->second && node->second->is_leaf() && node->second->pane == target)) {
        return node;
    }

    LayoutNode *found = find_parent(node->first.get(), target);
    if (found) return found;
    return find_parent(node->second.get(), target);
}

} // namespace rivt

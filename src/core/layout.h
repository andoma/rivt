#pragma once
#include "core/pane.h"
#include <memory>
#include <vector>

namespace rivt {

enum class SplitDir { Horizontal, Vertical };

// Direction for pane navigation
enum class NavDir { Up, Down, Left, Right };

struct LayoutNode {
    // Leaf node: pane pointer
    Pane *pane = nullptr;

    // Internal node: split direction, ratio, children
    SplitDir split_dir = SplitDir::Vertical;
    float ratio = 0.5f;
    std::unique_ptr<LayoutNode> first;
    std::unique_ptr<LayoutNode> second;

    bool is_leaf() const { return pane != nullptr; }

    // Geometry from the last compute_layout (for divider hit-testing).
    int x = 0, y = 0, w = 0, h = 0;
};

class LayoutTree {
public:
    LayoutTree();

    // Initialize with a single pane
    void init(Pane *pane);

    // Split the pane that contains target, placing new_pane in the given direction.
    // Returns true if the split was performed.
    bool split(Pane *target, Pane *new_pane, SplitDir dir);

    // Remove a pane from the layout, collapsing its parent split.
    // Returns true if the pane was found and removed.
    bool remove(Pane *target);

    // Recompute layout geometry given window area.
    // border_width is the pixel width of dividers between panes.
    // cell_w/cell_h are used to snap pane sizes to cell boundaries.
    void compute_layout(int x, int y, int w, int h, int border_width,
                        int cell_w, int cell_h);

    // Find the pane at pixel coordinates (px, py). Returns nullptr if none.
    Pane *pane_at(int px, int py) const;

    // Divider hit test: the split whose divider passes within `slop`
    // pixels of (px, py), or nullptr. Uses last compute_layout geometry.
    LayoutNode *divider_at(int px, int py, int slop);
    // Move a split's divider to the given pixel position along its axis
    // (ratio clamped so both sides keep at least ~one cell).
    static void drag_divider(LayoutNode *split, int px, int py);
    // Move target's trailing (right/bottom) divider by delta units along
    // the axis: adjusts the deepest ancestor split of the matching
    // orientation whose first subtree holds target. False if target has
    // no such divider (it sits on the container edge).
    bool resize_edge(Pane *target, bool horizontal, int delta);

    // Navigate from a pane in a direction. Returns the target pane, or from if no neighbor.
    Pane *navigate(Pane *from, NavDir dir) const;

    // Get all leaf panes in order
    void collect_panes(std::vector<Pane *> &out) const;

    // Get root node (for inspection)
    LayoutNode *root() { return m_root.get(); }

    // Replace the whole tree (daemon handover reconstructs it directly).
    void set_root(std::unique_ptr<LayoutNode> r) { m_root = std::move(r); }
    const LayoutNode *root() const { return m_root.get(); }

    bool empty() const { return !m_root; }

private:
    // Find the parent of the node containing target pane
    LayoutNode *find_parent(LayoutNode *node, Pane *target) const;
    LayoutNode *find_leaf(LayoutNode *node, Pane *target) const;

    void compute_node(LayoutNode *node, int x, int y, int w, int h,
                      int border_width, int cell_w, int cell_h);
    Pane *pane_at_node(const LayoutNode *node, int px, int py) const;
    void collect_node(const LayoutNode *node, std::vector<Pane *> &out) const;

    // For navigation: find pane center, then search for best match
    struct PaneCenter { Pane *pane; int cx, cy; };
    void collect_centers(const LayoutNode *node, std::vector<PaneCenter> &out) const;

    std::unique_ptr<LayoutNode> m_root;
};

} // namespace rivt

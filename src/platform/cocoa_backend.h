#pragma once
#include "platform/platform.h"

namespace rivt {

// Per-window Cocoa backend. The first instance lazily initializes a
// process-wide singleton (CocoaApp) that owns NSApp, the app delegate,
// the menu bar, and tracks the current key window for menu routing.
//
// This header is plain C++ so it can be included from non-Objective-C
// translation units (notably platform.cpp). All Objective-C state is
// stashed behind opaque pointers and managed in cocoa_backend.mm.
class CocoaBackend : public Platform {
public:
    CocoaBackend();
    ~CocoaBackend() override;

    bool create_window(int width, int height, const std::string &title) override;
    void destroy_window() override;
    void set_title(const std::string &title) override;
    void get_size(int &width, int &height) override;
    void resize_window(int width, int height) override;
    void show_window() override;
    void set_size_hints(int cell_w, int cell_h, int base_w, int base_h) override;
    int impl_cell_height() const;  // px; 0 until size hints arrive

    bool create_gl_context() override;
    void make_current() override;
    void swap_buffers() override;

    int get_event_fd() override { return -1; }
    void process_events() override {}  // NSApp dispatches centrally

    void set_clipboard(const std::string &text, bool primary) override;
    std::string get_clipboard(bool primary) override;
    void set_clipboard_data(const std::string &data, const std::string &mime_type, bool primary) override;
    std::string get_clipboard_data(const std::string &mime_type, bool primary) override;

    float get_dpi_scale() override;
    void set_mouse_cursor(MouseCursor cursor) override;

    // Internal hooks used by the Objective-C code in cocoa_backend.mm.
    // Not part of the Platform API.
    void notify_key_window_focus(bool focused);
    void _impl_set_size(int w, int h);
    void _impl_set_scale(float s);
    void _impl_update_gl_drawable();
    MouseCursor _impl_cursor() const;

private:
    struct Impl;
    Impl *m_impl = nullptr;
};

} // namespace rivt

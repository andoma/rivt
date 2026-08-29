#include "platform/platform.h"
#ifdef __APPLE__
#include "platform/cocoa_backend.h"
#else
#include "platform/x11_backend.h"
#endif

namespace rivt {

std::unique_ptr<Platform> Platform::create() {
#ifdef __APPLE__
    return std::make_unique<CocoaBackend>();
#else
    return std::make_unique<X11Backend>();
#endif
}

namespace {
std::function<void()> g_new_window_handler;
std::function<void()> g_quit_handler;
std::function<void(Platform::ConnEvent)> g_connectivity_handler;
} // namespace

void Platform::set_new_window_handler(std::function<void()> handler) {
    g_new_window_handler = std::move(handler);
}
void Platform::set_quit_handler(std::function<void()> handler) {
    g_quit_handler = std::move(handler);
}
const std::function<void()> &Platform::new_window_handler() {
    return g_new_window_handler;
}
void Platform::set_connectivity_handler(std::function<void(ConnEvent)> handler) {
    g_connectivity_handler = std::move(handler);
}
const std::function<void(Platform::ConnEvent)> &Platform::connectivity_handler() {
    return g_connectivity_handler;
}
const std::function<void()> &Platform::quit_handler() {
    return g_quit_handler;
}

} // namespace rivt

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

} // namespace rivt

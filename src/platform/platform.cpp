#include "platform/platform.h"
#include "platform/x11_backend.h"

namespace rivt {

std::unique_ptr<Platform> Platform::create() {
    return std::make_unique<X11Backend>();
}

} // namespace rivt

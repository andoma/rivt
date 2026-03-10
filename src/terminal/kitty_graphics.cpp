#include "terminal/kitty_graphics.h"
#include <cstdlib>

namespace rivt {

KittyGraphicsCommand parse_kitty_graphics(const std::string &data) {
    KittyGraphicsCommand cmd;

    // Format: key=val,key=val,...;base64payload
    // Find the semicolon separating key-value pairs from payload
    size_t semi = data.find(';');
    std::string kvs = (semi != std::string::npos) ? data.substr(0, semi) : data;
    if (semi != std::string::npos)
        cmd.payload = data.substr(semi + 1);

    // Parse comma-separated key=value pairs
    size_t pos = 0;
    while (pos < kvs.size()) {
        size_t comma = kvs.find(',', pos);
        std::string pair = kvs.substr(pos, comma != std::string::npos ? comma - pos : std::string::npos);
        pos = (comma != std::string::npos) ? comma + 1 : kvs.size();

        size_t eq = pair.find('=');
        if (eq == std::string::npos || eq == 0) continue;

        char key = pair[0];
        std::string val = pair.substr(eq + 1);

        switch (key) {
            case 'a': if (!val.empty()) cmd.action = val[0]; break;
            case 'f': cmd.format = std::atoi(val.c_str()); break;
            case 't': if (!val.empty()) cmd.transmission = val[0]; break;
            case 'i': cmd.image_id = (uint32_t)std::strtoul(val.c_str(), nullptr, 10); break;
            case 'p': cmd.placement_id = (uint32_t)std::strtoul(val.c_str(), nullptr, 10); break;
            case 's': cmd.src_width = std::atoi(val.c_str()); break;
            case 'v': cmd.src_height = std::atoi(val.c_str()); break;
            case 'c': cmd.columns = std::atoi(val.c_str()); break;
            case 'r': cmd.rows = std::atoi(val.c_str()); break;
            case 'z': cmd.z_index = std::atoi(val.c_str()); break;
            case 'm': cmd.more = std::atoi(val.c_str()); break;
            case 'C': cmd.no_move_cursor = (val == "1"); break;
            case 'q': cmd.quiet = std::atoi(val.c_str()); break;
            case 'd': if (!val.empty()) cmd.delete_target = val[0]; break;
            default: break;
        }
    }

    return cmd;
}

} // namespace rivt

#pragma once
#include <cstdint>
#include <string>

namespace rivt {

struct KittyGraphicsCommand {
    char action = 't';       // a= : t(ransmit), p(lace), d(elete), q(uery), T(ransmit+display)
    int format = 32;         // f= : 24=RGB, 32=RGBA, 100=PNG
    char transmission = 'd'; // t= : d(irect)
    uint32_t image_id = 0;   // i=
    uint32_t placement_id = 0; // p=
    int src_width = 0;       // s=
    int src_height = 0;      // v=
    int columns = 0;         // c=
    int rows = 0;            // r=
    int z_index = 0;         // z=
    int more = 0;            // m= : 0=last chunk, 1=more chunks follow
    bool no_move_cursor = false; // C=1
    int quiet = 0;           // q= : 0=normal, 1=suppress OK, 2=suppress all
    char delete_target = 'a'; // d= : a(ll), i(mage id), p(lacement)
    std::string payload;     // base64 data after semicolon
};

KittyGraphicsCommand parse_kitty_graphics(const std::string &data);

} // namespace rivt

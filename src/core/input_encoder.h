#pragma once
#include "core/types.h"
#include <string>

namespace rivt {

class ScreenBuffer;

// Encode a mouse event as an SGR or legacy X11 escape sequence.
std::string encode_mouse(const MouseEvent &mouse, int cell_col, int cell_row, bool sgr);

// Encode key event for the PTY — dispatches to kitty or legacy encoding
std::string encode_key(const KeyEvent &key, const ScreenBuffer &buffer);

// Kitty keyboard protocol encoding
std::string encode_key_kitty(const KeyEvent &key, const ScreenBuffer &buffer);

// Legacy terminal key encoding
std::string encode_key_legacy(const KeyEvent &key, const ScreenBuffer &buffer);

} // namespace rivt

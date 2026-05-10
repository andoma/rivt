#pragma once
//
// Platform-neutral keysym constants.
//
// On Linux this just pulls in xkbcommon's full keysym table. On macOS we
// don't link xkbcommon, so we define the subset the codebase actually uses,
// keeping the X11 numeric values so existing constants compare correctly
// against the values produced by the platform backend.
//
// Numeric values match X11/keysymdef.h verbatim — ASCII keysyms (0x20–0x7e)
// are the ASCII codepoint; non-ASCII keysyms live in the 0xff** namespace.
//

#ifndef __APPLE__

#include <xkbcommon/xkbcommon-keysyms.h>

#else

// ASCII-range keysyms (X11 keysym values equal the ASCII codepoint).
#define XKB_KEY_space        0x0020
#define XKB_KEY_plus         0x002b
#define XKB_KEY_minus        0x002d
#define XKB_KEY_slash        0x002f
#define XKB_KEY_0            0x0030
#define XKB_KEY_1            0x0031
#define XKB_KEY_9            0x0039
#define XKB_KEY_equal        0x003d
#define XKB_KEY_at           0x0040
#define XKB_KEY_A            0x0041
#define XKB_KEY_C            0x0043
#define XKB_KEY_D            0x0044
#define XKB_KEY_E            0x0045
#define XKB_KEY_F            0x0046
#define XKB_KEY_N            0x004e
#define XKB_KEY_T            0x0054
#define XKB_KEY_V            0x0056
#define XKB_KEY_W            0x0057
#define XKB_KEY_Z            0x005a
#define XKB_KEY_bracketleft  0x005b
#define XKB_KEY_backslash    0x005c
#define XKB_KEY_bracketright 0x005d
#define XKB_KEY_asciicircum  0x005e
#define XKB_KEY_underscore   0x005f
#define XKB_KEY_a            0x0061
#define XKB_KEY_c            0x0063
#define XKB_KEY_d            0x0064
#define XKB_KEY_e            0x0065
#define XKB_KEY_f            0x0066
#define XKB_KEY_n            0x006e
#define XKB_KEY_t            0x0074
#define XKB_KEY_v            0x0076
#define XKB_KEY_w            0x0077
#define XKB_KEY_z            0x007a
#define XKB_KEY_asciitilde   0x007e

// ISO function keys (0xfe**).
#define XKB_KEY_ISO_Left_Tab 0xfe20

// Function-namespace keysyms (0xff**).
#define XKB_KEY_BackSpace    0xff08
#define XKB_KEY_Tab          0xff09
#define XKB_KEY_Return       0xff0d
#define XKB_KEY_Pause        0xff13
#define XKB_KEY_Scroll_Lock  0xff14
#define XKB_KEY_Escape       0xff1b
#define XKB_KEY_Home         0xff50
#define XKB_KEY_Left         0xff51
#define XKB_KEY_Up           0xff52
#define XKB_KEY_Right        0xff53
#define XKB_KEY_Down         0xff54
#define XKB_KEY_Page_Up      0xff55
#define XKB_KEY_Page_Down    0xff56
#define XKB_KEY_End          0xff57
#define XKB_KEY_Print        0xff61
#define XKB_KEY_Insert       0xff63
#define XKB_KEY_Menu         0xff67
#define XKB_KEY_Num_Lock     0xff7f
#define XKB_KEY_KP_Enter     0xff8d
#define XKB_KEY_F1           0xffbe
#define XKB_KEY_F2           0xffbf
#define XKB_KEY_F3           0xffc0
#define XKB_KEY_F4           0xffc1
#define XKB_KEY_F5           0xffc2
#define XKB_KEY_F6           0xffc3
#define XKB_KEY_F7           0xffc4
#define XKB_KEY_F8           0xffc5
#define XKB_KEY_F9           0xffc6
#define XKB_KEY_F10          0xffc7
#define XKB_KEY_F11          0xffc8
#define XKB_KEY_F12          0xffc9
#define XKB_KEY_Shift_L      0xffe1
#define XKB_KEY_Shift_R      0xffe2
#define XKB_KEY_Control_L    0xffe3
#define XKB_KEY_Control_R    0xffe4
#define XKB_KEY_Caps_Lock    0xffe5
#define XKB_KEY_Meta_L       0xffe7
#define XKB_KEY_Meta_R       0xffe8
#define XKB_KEY_Alt_L        0xffe9
#define XKB_KEY_Alt_R        0xffea
#define XKB_KEY_Super_L      0xffeb
#define XKB_KEY_Super_R      0xffec
#define XKB_KEY_Hyper_L      0xffed
#define XKB_KEY_Hyper_R      0xffee
#define XKB_KEY_Delete       0xffff

#endif // __APPLE__

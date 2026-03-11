#include "terminal/vt_parser.h"
#include <cstring>

namespace rivt {

VtParser::VtParser(VtHandler &handler) : m_handler(handler) {}

void VtParser::transition(State new_state) {
    // Entry actions
    switch (new_state) {
        case State::CsiEntry:
            m_csi_param_str.clear();
            m_csi_intermediate = 0;
            break;
        case State::OscString:
            m_osc_string.clear();
            break;
        case State::Escape:
            m_esc_intermediate = 0;
            break;
        case State::DcsEntry:
            m_dcs_param_str.clear();
            break;
        case State::ApcString:
            m_apc_string.clear();
            break;
        default:
            break;
    }
    m_state = new_state;
}

void VtParser::action_print(uint32_t cp) {
    m_handler.print(cp);
}

void VtParser::action_execute(uint8_t byte) {
    m_handler.execute(byte);
}

void VtParser::parse_csi_params() {
    // no-op here, done in action_csi_dispatch
}

void VtParser::action_csi_dispatch(char final_byte) {
    CsiParams params;

    // Parse param string like "1;2" or "38:2::255:0:0;1"
    if (!m_csi_param_str.empty()) {
        const char *p = m_csi_param_str.c_str();
        while (*p) {
            CsiParam param;
            if (*p >= '0' && *p <= '9') {
                param.value = 0;
                while (*p >= '0' && *p <= '9') {
                    param.value = param.value * 10 + (*p - '0');
                    p++;
                }
            }
            // Sub-parameters (colon separated)
            while (*p == ':') {
                p++;
                int sub_val = -1;
                if (*p >= '0' && *p <= '9') {
                    sub_val = 0;
                    while (*p >= '0' && *p <= '9') {
                        sub_val = sub_val * 10 + (*p - '0');
                        p++;
                    }
                }
                param.sub.push_back(sub_val);
            }
            params.params.push_back(param);
            if (*p == ';') p++;
        }
    }

    m_handler.csi_dispatch(params, m_csi_intermediate, final_byte);
}

void VtParser::action_osc_dispatch() {
    int command = 0;
    std::string payload;

    size_t semi = m_osc_string.find(';');
    if (semi != std::string::npos) {
        // Parse command number
        for (size_t i = 0; i < semi; i++) {
            if (m_osc_string[i] >= '0' && m_osc_string[i] <= '9')
                command = command * 10 + (m_osc_string[i] - '0');
        }
        payload = m_osc_string.substr(semi + 1);
    }

    m_handler.osc_dispatch(command, payload);
}

void VtParser::action_esc_dispatch(char final_byte) {
    m_handler.esc_dispatch(m_esc_intermediate, final_byte);
}

void VtParser::feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        // Handle UTF-8 continuation bytes in any state that expects them
        if (m_state == State::Utf8) {
            if ((byte & 0xC0) == 0x80) {
                m_utf8_codepoint = (m_utf8_codepoint << 6) | (byte & 0x3F);
                m_utf8_remaining--;
                if (m_utf8_remaining == 0) {
                    action_print(m_utf8_codepoint);
                    m_state = State::Ground;
                }
                continue;
            } else {
                // Invalid continuation, abort and reprocess
                m_state = State::Ground;
                // fall through to reprocess this byte
            }
        }

        // C0 controls are handled in most states (anywhere transitions)
        // But NOT in OSC/DCS/APC states where ESC is part of ST (ESC \)
        if (byte == 0x1B && m_state != State::OscString && m_state != State::DcsPassthrough && m_state != State::ApcString) {
            transition(State::Escape);
            continue;
        }

        // In Ground or when applicable, handle C0 controls
        if (byte < 0x20 && byte != 0x1B && m_state != State::OscString && m_state != State::DcsPassthrough && m_state != State::ApcString) {
            if (byte == 0x07 && m_state == State::OscString) {
                // BEL terminates OSC (handled below)
            } else if (byte < 0x20) {
                action_execute(byte);
                continue;
            }
        }

        switch (m_state) {
            case State::Ground:
                if (byte >= 0x20 && byte <= 0x7E) {
                    action_print(byte);
                } else if (byte >= 0xC0 && byte <= 0xDF) {
                    m_utf8_codepoint = byte & 0x1F;
                    m_utf8_remaining = 1;
                    m_state = State::Utf8;
                } else if (byte >= 0xE0 && byte <= 0xEF) {
                    m_utf8_codepoint = byte & 0x0F;
                    m_utf8_remaining = 2;
                    m_state = State::Utf8;
                } else if (byte >= 0xF0 && byte <= 0xF7) {
                    m_utf8_codepoint = byte & 0x07;
                    m_utf8_remaining = 3;
                    m_state = State::Utf8;
                }
                // bytes 0x80-0xBF, 0xF8-0xFF: ignore
                break;

            case State::Escape:
                if (byte == '[') {
                    transition(State::CsiEntry);
                } else if (byte == ']') {
                    transition(State::OscString);
                } else if (byte == 'P') {
                    transition(State::DcsEntry);
                    m_handler.dcs_start();
                } else if (byte == '_') {
                    transition(State::ApcString);
                } else if (byte >= 0x20 && byte <= 0x2F) {
                    m_esc_intermediate = byte;
                    m_state = State::EscapeIntermediate;
                } else if (byte >= 0x30 && byte <= 0x7E) {
                    action_esc_dispatch(byte);
                    m_state = State::Ground;
                } else {
                    m_state = State::Ground;
                }
                break;

            case State::EscapeIntermediate:
                if (byte >= 0x20 && byte <= 0x2F) {
                    // Collect more intermediates (we only keep last)
                    m_esc_intermediate = byte;
                } else if (byte >= 0x30 && byte <= 0x7E) {
                    action_esc_dispatch(byte);
                    m_state = State::Ground;
                } else {
                    m_state = State::Ground;
                }
                break;

            case State::CsiEntry:
                if (byte >= '0' && byte <= '9') {
                    m_csi_param_str += (char)byte;
                    m_state = State::CsiParam;
                } else if (byte == ';' || byte == ':') {
                    m_csi_param_str += (char)byte;
                    m_state = State::CsiParam;
                } else if (byte == '?') {
                    // Private mode indicator — store as intermediate
                    m_csi_intermediate = byte;
                    m_state = State::CsiParam;
                } else if (byte == '>') {
                    m_csi_intermediate = byte;
                    m_state = State::CsiParam;
                } else if (byte == '<') {
                    m_csi_intermediate = byte;
                    m_state = State::CsiParam;
                } else if (byte >= 0x40 && byte <= 0x7E) {
                    action_csi_dispatch(byte);
                    m_state = State::Ground;
                } else if (byte >= 0x20 && byte <= 0x2F) {
                    m_csi_intermediate = byte;
                    m_state = State::CsiIntermediate;
                } else {
                    m_state = State::CsiIgnore;
                }
                break;

            case State::CsiParam:
                if (byte >= '0' && byte <= '9') {
                    m_csi_param_str += (char)byte;
                } else if (byte == ';' || byte == ':') {
                    m_csi_param_str += (char)byte;
                } else if (byte >= 0x40 && byte <= 0x7E) {
                    action_csi_dispatch(byte);
                    m_state = State::Ground;
                } else if (byte >= 0x20 && byte <= 0x2F) {
                    m_csi_intermediate = byte;
                    m_state = State::CsiIntermediate;
                } else {
                    m_state = State::CsiIgnore;
                }
                break;

            case State::CsiIntermediate:
                if (byte >= 0x20 && byte <= 0x2F) {
                    // ignore extra intermediates
                } else if (byte >= 0x40 && byte <= 0x7E) {
                    action_csi_dispatch(byte);
                    m_state = State::Ground;
                } else {
                    m_state = State::CsiIgnore;
                }
                break;

            case State::CsiIgnore:
                if (byte >= 0x40 && byte <= 0x7E) {
                    m_state = State::Ground;
                }
                break;

            case State::OscString:
                if (byte == 0x07) {
                    // BEL terminates OSC
                    action_osc_dispatch();
                    m_state = State::Ground;
                } else if (byte == 0x1B) {
                    // Will be caught by ST (ESC \)
                    // Peek at next byte
                    if (i + 1 < len && data[i + 1] == '\\') {
                        i++;  // consume the backslash
                        action_osc_dispatch();
                        m_state = State::Ground;
                    }
                } else if (m_osc_string.size() < 16 * 1024 * 1024) {
                    m_osc_string += (char)byte;
                }
                break;

            case State::ApcString:
                if (byte == 0x07) {
                    m_handler.apc_dispatch(m_apc_string);
                    m_state = State::Ground;
                } else if (byte == 0x1B) {
                    if (i + 1 < len && data[i + 1] == '\\') {
                        i++;
                        m_handler.apc_dispatch(m_apc_string);
                        m_state = State::Ground;
                    }
                } else if (m_apc_string.size() < 32 * 1024 * 1024) {
                    m_apc_string += (char)byte;
                }
                break;

            case State::DcsEntry:
                if (byte >= 0x40 && byte <= 0x7E) {
                    m_state = State::DcsPassthrough;
                } else if (byte >= 0x30 && byte <= 0x3F) {
                    m_dcs_param_str += (char)byte;
                } else {
                    m_state = State::DcsPassthrough;
                }
                break;

            case State::DcsPassthrough:
                if (byte == 0x1B) {
                    if (i + 1 < len && data[i + 1] == '\\') {
                        i++;
                        m_handler.dcs_end();
                        m_state = State::Ground;
                    } else {
                        m_handler.dcs_put(byte);
                    }
                } else {
                    m_handler.dcs_put(byte);
                }
                break;

            case State::DcsIgnore:
                if (byte == 0x1B) {
                    if (i + 1 < len && data[i + 1] == '\\') {
                        i++;
                        m_state = State::Ground;
                    }
                }
                break;

            default:
                m_state = State::Ground;
                break;
        }
    }
}

} // namespace rivt

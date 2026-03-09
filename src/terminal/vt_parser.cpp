#include "terminal/vt_parser.h"
#include <cstring>

namespace rivt {

VtParser::VtParser(VtHandler &handler) : handler_(handler) {}

void VtParser::transition(State new_state) {
    // Entry actions
    switch (new_state) {
        case State::CsiEntry:
            csi_param_str_.clear();
            csi_intermediate_ = 0;
            break;
        case State::OscString:
            osc_string_.clear();
            break;
        case State::Escape:
            esc_intermediate_ = 0;
            break;
        case State::DcsEntry:
            dcs_param_str_.clear();
            break;
        default:
            break;
    }
    state_ = new_state;
}

void VtParser::action_print(uint32_t cp) {
    handler_.print(cp);
}

void VtParser::action_execute(uint8_t byte) {
    handler_.execute(byte);
}

void VtParser::parse_csi_params() {
    // no-op here, done in action_csi_dispatch
}

void VtParser::action_csi_dispatch(char final_byte) {
    CsiParams params;

    // Parse param string like "1;2" or "38:2::255:0:0;1"
    if (!csi_param_str_.empty()) {
        const char *p = csi_param_str_.c_str();
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

    handler_.csi_dispatch(params, csi_intermediate_, final_byte);
}

void VtParser::action_osc_dispatch() {
    int command = 0;
    std::string payload;

    size_t semi = osc_string_.find(';');
    if (semi != std::string::npos) {
        // Parse command number
        for (size_t i = 0; i < semi; i++) {
            if (osc_string_[i] >= '0' && osc_string_[i] <= '9')
                command = command * 10 + (osc_string_[i] - '0');
        }
        payload = osc_string_.substr(semi + 1);
    }

    handler_.osc_dispatch(command, payload);
}

void VtParser::action_esc_dispatch(char final_byte) {
    handler_.esc_dispatch(esc_intermediate_, final_byte);
}

void VtParser::feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        // Handle UTF-8 continuation bytes in any state that expects them
        if (state_ == State::Utf8) {
            if ((byte & 0xC0) == 0x80) {
                utf8_codepoint_ = (utf8_codepoint_ << 6) | (byte & 0x3F);
                utf8_remaining_--;
                if (utf8_remaining_ == 0) {
                    action_print(utf8_codepoint_);
                    state_ = State::Ground;
                }
                continue;
            } else {
                // Invalid continuation, abort and reprocess
                state_ = State::Ground;
                // fall through to reprocess this byte
            }
        }

        // C0 controls are handled in most states (anywhere transitions)
        // But NOT in OSC/DCS states where ESC is part of ST (ESC \)
        if (byte == 0x1B && state_ != State::OscString && state_ != State::DcsPassthrough) {
            transition(State::Escape);
            continue;
        }

        // In Ground or when applicable, handle C0 controls
        if (byte < 0x20 && byte != 0x1B && state_ != State::OscString && state_ != State::DcsPassthrough) {
            if (byte == 0x07 && state_ == State::OscString) {
                // BEL terminates OSC (handled below)
            } else if (byte < 0x20) {
                action_execute(byte);
                continue;
            }
        }

        switch (state_) {
            case State::Ground:
                if (byte >= 0x20 && byte <= 0x7E) {
                    action_print(byte);
                } else if (byte >= 0xC0 && byte <= 0xDF) {
                    utf8_codepoint_ = byte & 0x1F;
                    utf8_remaining_ = 1;
                    state_ = State::Utf8;
                } else if (byte >= 0xE0 && byte <= 0xEF) {
                    utf8_codepoint_ = byte & 0x0F;
                    utf8_remaining_ = 2;
                    state_ = State::Utf8;
                } else if (byte >= 0xF0 && byte <= 0xF7) {
                    utf8_codepoint_ = byte & 0x07;
                    utf8_remaining_ = 3;
                    state_ = State::Utf8;
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
                    handler_.dcs_start();
                } else if (byte >= 0x20 && byte <= 0x2F) {
                    esc_intermediate_ = byte;
                    state_ = State::EscapeIntermediate;
                } else if (byte >= 0x30 && byte <= 0x7E) {
                    action_esc_dispatch(byte);
                    state_ = State::Ground;
                } else {
                    state_ = State::Ground;
                }
                break;

            case State::EscapeIntermediate:
                if (byte >= 0x20 && byte <= 0x2F) {
                    // Collect more intermediates (we only keep last)
                    esc_intermediate_ = byte;
                } else if (byte >= 0x30 && byte <= 0x7E) {
                    action_esc_dispatch(byte);
                    state_ = State::Ground;
                } else {
                    state_ = State::Ground;
                }
                break;

            case State::CsiEntry:
                if (byte >= '0' && byte <= '9') {
                    csi_param_str_ += (char)byte;
                    state_ = State::CsiParam;
                } else if (byte == ';' || byte == ':') {
                    csi_param_str_ += (char)byte;
                    state_ = State::CsiParam;
                } else if (byte == '?') {
                    // Private mode indicator — store as intermediate
                    csi_intermediate_ = byte;
                    state_ = State::CsiParam;
                } else if (byte == '>') {
                    csi_intermediate_ = byte;
                    state_ = State::CsiParam;
                } else if (byte == '<') {
                    csi_intermediate_ = byte;
                    state_ = State::CsiParam;
                } else if (byte >= 0x40 && byte <= 0x7E) {
                    action_csi_dispatch(byte);
                    state_ = State::Ground;
                } else if (byte >= 0x20 && byte <= 0x2F) {
                    csi_intermediate_ = byte;
                    state_ = State::CsiIntermediate;
                } else {
                    state_ = State::CsiIgnore;
                }
                break;

            case State::CsiParam:
                if (byte >= '0' && byte <= '9') {
                    csi_param_str_ += (char)byte;
                } else if (byte == ';' || byte == ':') {
                    csi_param_str_ += (char)byte;
                } else if (byte >= 0x40 && byte <= 0x7E) {
                    action_csi_dispatch(byte);
                    state_ = State::Ground;
                } else if (byte >= 0x20 && byte <= 0x2F) {
                    csi_intermediate_ = byte;
                    state_ = State::CsiIntermediate;
                } else {
                    state_ = State::CsiIgnore;
                }
                break;

            case State::CsiIntermediate:
                if (byte >= 0x20 && byte <= 0x2F) {
                    // ignore extra intermediates
                } else if (byte >= 0x40 && byte <= 0x7E) {
                    action_csi_dispatch(byte);
                    state_ = State::Ground;
                } else {
                    state_ = State::CsiIgnore;
                }
                break;

            case State::CsiIgnore:
                if (byte >= 0x40 && byte <= 0x7E) {
                    state_ = State::Ground;
                }
                break;

            case State::OscString:
                if (byte == 0x07) {
                    // BEL terminates OSC
                    action_osc_dispatch();
                    state_ = State::Ground;
                } else if (byte == 0x1B) {
                    // Will be caught by ST (ESC \)
                    // Peek at next byte
                    if (i + 1 < len && data[i + 1] == '\\') {
                        i++;  // consume the backslash
                        action_osc_dispatch();
                        state_ = State::Ground;
                    }
                } else {
                    osc_string_ += (char)byte;
                }
                break;

            case State::DcsEntry:
                if (byte >= 0x40 && byte <= 0x7E) {
                    state_ = State::DcsPassthrough;
                } else if (byte >= 0x30 && byte <= 0x3F) {
                    dcs_param_str_ += (char)byte;
                } else {
                    state_ = State::DcsPassthrough;
                }
                break;

            case State::DcsPassthrough:
                if (byte == 0x1B) {
                    if (i + 1 < len && data[i + 1] == '\\') {
                        i++;
                        handler_.dcs_end();
                        state_ = State::Ground;
                    } else {
                        handler_.dcs_put(byte);
                    }
                } else {
                    handler_.dcs_put(byte);
                }
                break;

            case State::DcsIgnore:
                if (byte == 0x1B) {
                    if (i + 1 < len && data[i + 1] == '\\') {
                        i++;
                        state_ = State::Ground;
                    }
                }
                break;

            default:
                state_ = State::Ground;
                break;
        }
    }
}

} // namespace rivt

#include "tmux/tmux_client.h"
#include "core/debug.h"

#include <cstdio>

namespace rivt {

TmuxClient::TmuxClient(EventLoop &loop) : m_loop(loop) {}

TmuxClient::~TmuxClient() = default;

void TmuxClient::start_pty_mode(std::function<void(const std::string &)> write_fn) {
    m_pty_write = std::move(write_fn);
}

void TmuxClient::feed_data(const char *buf, size_t len) {
    m_line_buf.append(buf, len);

    size_t pos;
    while ((pos = m_line_buf.find('\n')) != std::string::npos) {
        std::string line = m_line_buf.substr(0, pos);
        m_line_buf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        parse_line(line);
    }
}

void TmuxClient::parse_line(const std::string &line) {
    if (line.empty()) return;

    // Log non-output lines (output is too frequent)
    if (line[0] == '%' && line.substr(0, 7) != "%output") {
        dbg("tmux-client: << %s", line.c_str());
    }

    // Handle %begin/%end/%error blocks
    if (m_in_block) {
        if (line.substr(0, 4) == "%end" || line.substr(0, 6) == "%error") {
            m_in_block = false;
            if (!m_response_queue.empty()) {
                auto cb = std::move(m_response_queue.front());
                m_response_queue.pop_front();
                if (cb) cb(m_block_output);
            }
            m_block_output.clear();
            return;
        }
        m_block_output += line + "\n";
        return;
    }

    if (line.substr(0, 6) == "%begin") {
        m_in_block = true;
        m_block_output.clear();
        return;
    }

    if (line.substr(0, 7) == "%output") {
        // %output %PANE_ID DATA
        // Find pane ID after "%output "
        size_t p = 8; // skip "%output "
        if (p >= line.size() || line[p] != '%') return;
        p++; // skip '%'
        size_t id_end = line.find(' ', p);
        if (id_end == std::string::npos) return;
        int pane_id = std::stoi(line.substr(p, id_end - p));
        std::string data = decode_octal(line.substr(id_end + 1));
        if (on_output) on_output(pane_id, data);
        return;
    }

    if (line.substr(0, 11) == "%window-add") {
        // %window-add @ID
        size_t p = 12;
        if (p >= line.size() || line[p] != '@') return;
        int window_id = std::stoi(line.substr(p + 1));
        if (on_window_add) on_window_add(window_id);
        return;
    }

    if (line.substr(0, 13) == "%window-close" ||
        line.substr(0, 22) == "%unlinked-window-close") {
        // %window-close @ID  or  %unlinked-window-close @ID
        size_t at = line.find('@');
        if (at == std::string::npos) return;
        int window_id = std::stoi(line.substr(at + 1));
        if (on_window_close) on_window_close(window_id);
        return;
    }

    if (line.substr(0, 15) == "%window-renamed" ||
        line.substr(0, 24) == "%unlinked-window-renamed") {
        // %window-renamed @ID name  or  %unlinked-window-renamed @ID name
        size_t at = line.find('@');
        if (at == std::string::npos) return;
        size_t id_end = line.find(' ', at + 1);
        if (id_end == std::string::npos) return;
        int window_id = std::stoi(line.substr(at + 1, id_end - at - 1));
        std::string name = line.substr(id_end + 1);
        if (on_window_renamed) on_window_renamed(window_id, name);
        return;
    }

    if (line.substr(0, 16) == "%session-changed") {
        if (on_session_changed) on_session_changed();
        return;
    }

    if (line.substr(0, 23) == "%session-window-changed") {
        // %session-window-changed $SESSION @WINDOW
        size_t at = line.find('@');
        if (at == std::string::npos) return;
        int window_id = std::stoi(line.substr(at + 1));
        if (on_session_window_changed) on_session_window_changed(window_id);
        return;
    }

    if (line.substr(0, 14) == "%layout-change") {
        // %layout-change @ID layout_string visible_layout_string [flags]
        size_t p = 15;
        if (p >= line.size() || line[p] != '@') return;
        size_t id_end = line.find(' ', p + 1);
        if (id_end == std::string::npos) return;
        int window_id = std::stoi(line.substr(p + 1, id_end - p - 1));
        // Layout string is the next token
        size_t layout_end = line.find(' ', id_end + 1);
        std::string layout;
        if (layout_end == std::string::npos)
            layout = line.substr(id_end + 1);
        else
            layout = line.substr(id_end + 1, layout_end - id_end - 1);
        // Check for active window flag (*) at end of line
        bool is_active = !line.empty() && line.back() == '*';
        if (on_layout_change) on_layout_change(window_id, layout, is_active);
        return;
    }

    if (line.substr(0, 5) == "%exit") {
        if (on_exit) on_exit();
        return;
    }
}

std::string TmuxClient::decode_octal(const std::string &s) {
    std::string result;
    result.reserve(s.size());

    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i+1] >= '0' && s[i+1] <= '3' &&
            s[i+2] >= '0' && s[i+2] <= '7' &&
            s[i+3] >= '0' && s[i+3] <= '7') {
            unsigned char c = (s[i+1] - '0') * 64 + (s[i+2] - '0') * 8 + (s[i+3] - '0');
            result += (char)c;
            i += 3;
        } else if (s[i] == '\\' && i + 1 < s.size() && s[i+1] == '\\') {
            result += '\\';
            i++;
        } else {
            result += s[i];
        }
    }
    return result;
}

void TmuxClient::send_command(const std::string &cmd, ResponseCallback cb) {
    dbg("tmux-client: >> %s", cmd.c_str());
    m_response_queue.push_back(std::move(cb));
    if (m_pty_write) m_pty_write(cmd + "\n");
}

void TmuxClient::send_keys(int pane_id, const std::string &data) {
    if (data.empty() || !m_pty_write) return;

    std::string cmd = "send-keys -t %" + std::to_string(pane_id) + " -H";
    for (unsigned char c : data) {
        char hex[4];
        snprintf(hex, sizeof(hex), " %02x", c);
        cmd += hex;
    }
    send_command(cmd);
}

void TmuxClient::refresh_client_size(int cols, int rows) {
    send_command("refresh-client -C " + std::to_string(cols) + "x" + std::to_string(rows));
}

void TmuxClient::detach() {
    send_command("detach");
}

} // namespace rivt

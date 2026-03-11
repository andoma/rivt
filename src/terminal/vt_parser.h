#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace rivt {

// CSI parameter with sub-parameter support (colon-separated)
struct CsiParam {
    int value = -1;  // -1 = default/empty
    std::vector<int> sub;  // colon-separated sub-parameters

    int get(int def = 0) const { return value < 0 ? def : value; }
};

struct CsiParams {
    std::vector<CsiParam> params;

    int get(int idx, int def = 0) const {
        if (idx < 0 || idx >= (int)params.size()) return def;
        return params[idx].get(def);
    }

    int count() const { return (int)params.size(); }
};

class VtHandler {
public:
    virtual ~VtHandler() = default;

    virtual void print(uint32_t codepoint) = 0;
    virtual void execute(uint8_t code) = 0;
    virtual void csi_dispatch(const CsiParams &params, char intermediate, char final_byte) = 0;
    virtual void osc_dispatch(int command, const std::string &payload) = 0;
    virtual void esc_dispatch(char intermediate, char final_byte) = 0;
    virtual void dcs_start() {}
    virtual void dcs_put(uint8_t byte) { (void)byte; }
    virtual void dcs_end() {}
    virtual void apc_dispatch(const std::string &payload) { (void)payload; }
};

class VtParser {
public:
    explicit VtParser(VtHandler &handler);

    void feed(const uint8_t *data, size_t len);
    void feed(const char *data, size_t len) {
        feed(reinterpret_cast<const uint8_t *>(data), len);
    }

private:
    enum class State {
        Ground,
        Escape,
        EscapeIntermediate,
        CsiEntry,
        CsiParam,
        CsiIntermediate,
        CsiIgnore,
        OscString,
        DcsEntry,
        DcsPassthrough,
        DcsIgnore,
        ApcString,
        Utf8,
    };

    void transition(State new_state);
    void action_print(uint32_t cp);
    void action_execute(uint8_t byte);
    void action_csi_dispatch(char final_byte);
    void action_osc_dispatch();
    void action_esc_dispatch(char final_byte);

    void parse_csi_params();

    VtHandler &m_handler;
    State m_state = State::Ground;

    // UTF-8 decoding state
    uint32_t m_utf8_codepoint = 0;
    int m_utf8_remaining = 0;

    // CSI accumulation
    std::string m_csi_param_str;
    char m_csi_intermediate = 0;

    // ESC intermediate
    char m_esc_intermediate = 0;

    // OSC accumulation
    std::string m_osc_string;

    // APC accumulation
    std::string m_apc_string;

    // DCS
    std::string m_dcs_param_str;
};

} // namespace rivt

// hostctl rendering (see tui.hpp)

#include "tui.hpp"
#include "error_host.hpp"
#include "tui_format.hpp"
#include <ftxui/dom/canvas.hpp>
#include <ftxui/screen/color.hpp>
#include <algorithm>
#include <cmath>
#include <format>

using namespace ftxui;
using namespace tuifmt;

namespace {

const Color kSeriesColors[] = {Color::Cyan, Color::Yellow, Color::Magenta, Color::Green};

Element row(const std::string &s) {
    return text(" " + s);
}

Element panel(const std::string &title, Elements rows) {
    return window(text(" " + title + " ") | bold, vbox(std::move(rows)));
}

// " label   value" with the label in a fixed column
Element field(const std::string &label, const std::string &value) {
    return hbox({text(" " + label) | size(WIDTH, EQUAL, 10) | dim, text(value)});
}

Color level_color(DeviceLogLevel level) {
    switch (level) {
        case DeviceLogLevel_DEVICE_LOG_ERROR: return Color::Red;
        case DeviceLogLevel_DEVICE_LOG_WARN:  return Color::Yellow;
        case DeviceLogLevel_DEVICE_LOG_INFO:  return Color::Default;
        case DeviceLogLevel_DEVICE_LOG_DEBUG: return Color::GrayLight;
        default:                              return Color::GrayDark;
    }
}

std::string age(std::time_t now, std::time_t then) {
    return then ? std::format("{}s", now - then) : std::string("never");
}

} // namespace

// =============================================================================
// Frame
// =============================================================================

Element Tui::render() noexcept {
    const auto state = connector.get_state();
    Element body;
    switch (tab.load()) {
        case Tab::Overview: body = render_overview(state); break;
        case Tab::Power:    body = render_power(state); break;
        case Tab::Scope:    body = render_scope(state); break;
        case Tab::Log:      body = render_log(); break;
        case Tab::Device:   body = render_device(state); break;
    }
    Elements rows{render_tab_bar(state), body | flex};
    rows.push_back(render_messages(message_pane_lines()));
    rows.push_back(render_command_line());
    Element main = vbox(std::move(rows));
    if (form) {
        main = dbox({main, render_form() | clear_under | center});
    }
    return main;
}

Element Tui::render_tab_bar(const Connector::StateSnapshot &state) const {
    static constexpr const char *kTitles[kTabCount] = {"F1 Overview", "F2 Power", "F3 Scope", "F4 Log", "F5 Device"};
    Elements tabs;
    for (int i = 0; i < kTabCount; ++i) {
        Element t = text(" " + std::string(kTitles[i]) + " ");
        tabs.push_back(static_cast<int>(tab.load()) == i ? t | inverted | bold : t | dim);
        tabs.push_back(text("│") | dim);
    }
    const std::time_t now = std::time(nullptr);
    const auto progress = scope_client.progress();
    Elements right;
    if (progress.active) {
        const float frac = progress.total ? static_cast<float>(progress.received) / progress.total : 0.0f;
        right.push_back(text(" read ") | color(Color::Cyan));
        right.push_back(gauge(frac) | size(WIDTH, EQUAL, 12) | color(Color::Cyan));
        right.push_back(text(std::format(" {:3.0f}% ", frac * 100.0f)));
    }
    const bool stale = state.last_status == 0 || now - state.last_status > 5;
    right.push_back(text(" status " + age(now, state.last_status) + " ") | (stale ? color(Color::Red) : color(Color::Green)));
    right.push_back(text(now - last_heartbeat.load() == 0 ? "● " : "○ "));
    tabs.push_back(filler());
    for (auto &e : right) tabs.push_back(std::move(e));
    return hbox(std::move(tabs)) | border;
}

Element Tui::render_messages(size_t lines) const {
    Elements out;
    size_t total = 0, scroll = 0;
    {
        std::lock_guard lock(data_mutex);
        total = messages.size();
        scroll = std::min(message_scroll, total);
        const size_t end = total - scroll;
        const size_t begin = end > lines ? end - lines : 0;
        for (size_t i = begin; i < end; ++i) {
            const std::string &m = messages[i];
            Element e = text(" " + m);
            if (m.starts_with("dev ERROR") || m.starts_with("Controller:") || m.find("failed") != std::string::npos) {
                e = e | color(Color::Red);
            } else if (m.starts_with("dev WARN")) {
                e = e | color(Color::Yellow);
            } else if (m.starts_with("> ")) {
                e = e | dim;
            }
            out.push_back(std::move(e));
        }
    }
    while (out.size() < lines) {
        out.insert(out.begin(), text(""));
    }
    // Scroll position indicator on the right edge
    const float frac = total > lines ? 1.0f - static_cast<float>(scroll) / static_cast<float>(total - lines) : 1.0f;
    auto bar = gaugeUp(frac) | size(WIDTH, EQUAL, 1) | dim;
    std::string title = std::format(" Messages ({}) ", total);
    std::string help = scroll ? std::format(" ↑ {} older · Ctrl-↓/wheel newer · Esc newest ", scroll)
                              : " Ctrl-↑/wheel scroll · F6 resize ";
    auto header = hbox({text(title) | bold, filler(), text(help) | (scroll ? color(Color::Yellow) : dim)});
    return window(header, hbox({vbox(std::move(out)) | flex, total > lines ? bar : text("")})) |
           size(HEIGHT, EQUAL, static_cast<int>(lines) + 2) | reflect(message_box);
}

Element Tui::render_command_line() const {
    Element line = hbox({text(" > ") | bold, text(input), text(" ") | inverted});
    const std::string help = input.empty() ? "Tab completes · Up/Down history · F1-F5 views · 'help' · F6 messages · Ctrl-C or 'quit' exits" : hint;
    return vbox({line, text(" " + help) | dim});
}

// =============================================================================
// Panels
// =============================================================================

Element Tui::charger_panel(const Connector::StateSnapshot &state) const {
    const SystemStatus &ss = state.system_status;
    Elements rows;
    if (ss.has_charger) {
        const auto &c = ss.charger;
        rows.push_back(hbox({text(c.enabled ? " CHG ON " : " CHG OFF ") | (c.enabled ? color(Color::Green) : dim),
                             text(c.fault_present ? " FAULT" : "") | color(Color::Red),
                             text(c.battery_present ? "" : " NO BAT") | color(Color::Yellow)}));
        rows.push_back(row(std::string("State: ") + charge_status_name(c.charge_status)));
        rows.push_back(row(std::string("Input: ") + input_source_name(c.input_source)));
        rows.push_back(row(std::string("VBUS:  ") + vbus_status_name(c.vbus_status)));
        if (ss.has_usb_pd) {
            const bool contract = ss.usb_pd.state == UsbPdState_USB_PD_STATE_CONTRACT;
            rows.push_back(row("USB-C: " + usb_pd_summary(ss.usb_pd)) | (contract ? color(Color::Green) : nothing));
        }
        if (c.has_status_flags) {
            const bool dock = charger_inputs(c.status_flags).dock;
            std::string line = std::string("Dock:  ") + (dock ? "present" : "absent");
            if (ss.has_charger_config && ss.charger_config.has_dock_input_current_limit) {
                line += std::format(", limit {:.2f} A", ss.charger_config.dock_input_current_limit);
            }
            rows.push_back(row(line) | (dock ? nothing : dim));
        }
        if (c.has_fault_flags) {
            const auto f = charger_faults(c.fault_flags);
            if (!f.empty()) {
                rows.push_back(row("Fault: " + f) | color(Color::Red));
            }
        }
        if (c.has_status_flags) {
            const auto l = charger_limits(c.status_flags, c.has_fault_flags ? c.fault_flags : 0);
            if (!l.empty()) {
                rows.push_back(row("Limit: " + l) | color(Color::Yellow));
            }
            const auto w = charger_warnings(c.status_flags);
            if (!w.empty()) {
                rows.push_back(row("Warn:  " + w) | color(Color::Yellow));
            }
        }
    } else {
        rows.push_back(row("no status") | dim);
    }
    if (ss.has_charger_measurements) {
        const auto &m = ss.charger_measurements;
        rows.push_back(row(std::format("VBUS {:6.2f} V  IBUS {:6.2f} A", m.vbus_voltage, m.ibus_current)));
        rows.push_back(row(std::format("VBAT {:6.2f} V  IBAT {:6.2f} A", m.vbat_voltage, m.has_ibat_current ? m.ibat_current : 0.0f)));
        rows.push_back(row(std::format("VSYS {:6.2f} V", m.vsys_voltage)));
        if (m.has_vac1_voltage || m.has_vac2_voltage) {
            rows.push_back(row(std::format("VAC1 {:6.2f} V  VAC2 {:6.2f} V", m.vac1_voltage, m.vac2_voltage)));
        }
    }
    if (ss.has_charger_config) {
        const auto &cfg = ss.charger_config;
        rows.push_back(row(std::format("Limit chg {:.2f} V {:.2f} A", cfg.charge_voltage_limit, cfg.charge_current_limit)) | dim);
        rows.push_back(row(std::format("Limit in  {:.2f} V {:.2f} A", cfg.input_voltage_limit, cfg.input_current_limit)) | dim);
    }
    return panel("Charger", std::move(rows));
}

Element Tui::output_panel(const Connector::StateSnapshot &state) const {
    const SystemStatus &ss = state.system_status;
    Elements rows;
    if (ss.has_output_regulator) {
        const auto &o = ss.output_regulator;
        rows.push_back(hbox({text(o.enabled ? " OUT ON " : " OUT OFF ") | (o.enabled ? color(Color::Green) : dim),
                             text(std::string(o.scp_fault ? " SCP" : "") + (o.ocp_fault ? " OCP" : "") +
                                  (o.ovp_fault ? " OVP" : "")) | color(Color::Red)}));
        rows.push_back(row(std::string("Mode: ") + regulator_mode_name(o.operating_mode) +
                           (o.feedback_mode == OutputRegulatorFeedbackMode_OUTPUT_REGULATOR_FEEDBACK_MODE_PFM ? " PFM" : " PWM")));
    } else {
        rows.push_back(row("no status") | dim);
    }
    if (ss.has_output_regulator_config) {
        const auto &cfg = ss.output_regulator_config;
        rows.push_back(row(std::format("Set  {:6.2f} V", cfg.output_voltage)));
        rows.push_back(row(std::format("Ilim {:6.2f} A", cfg.output_current_limit)));
        rows.push_back(row(std::format("Rsns {:.3f} Ω", cfg.sense_resistor_ohms)) | dim);
    }
    rows.push_back(row("(setpoints: no ADC)") | dim);
    return panel("Output Regulator", std::move(rows));
}

Element Tui::usb_pd_panel(const Connector::StateSnapshot &state) const {
    const SystemStatus &ss = state.system_status;
    Elements rows;
    if (ss.has_usb_pd) {
        const auto &p = ss.usb_pd;
        const Color state_color = p.state == UsbPdState_USB_PD_STATE_CONTRACT     ? Color::Green
                                  : p.state == UsbPdState_USB_PD_STATE_HARD_RESET ? Color::Yellow
                                  : p.state == UsbPdState_USB_PD_STATE_ATTACHED   ? Color::Cyan
                                                                                  : Color::GrayLight;
        std::string st = usb_pd_state_name(p.state);
        if (p.has_spec_revision && p.spec_revision && p.state == UsbPdState_USB_PD_STATE_CONTRACT) {
            st += std::format(" (PD {}.0)", p.spec_revision);
        }
        rows.push_back(hbox({text(" State") | size(WIDTH, EQUAL, 10) | dim, text(st) | color(state_color) | bold}));
        std::string role = usb_pd_power_role_name(p.power_role);
        if (p.has_data_role && p.data_role != UsbPdDataRole_USB_PD_DATA_ROLE_NONE) {
            role += std::string(" / ") + usb_pd_data_role_name(p.data_role);
        }
        if (p.has_cc_line && p.cc_line) role += std::format(" · CC{}", p.cc_line);
        rows.push_back(field("Role", role));
        if (p.has_typec_current && p.typec_current != TypeCCurrent_TYPEC_CURRENT_NONE) {
            rows.push_back(field("Rp", typec_current_name(p.typec_current)));
        }
        if (p.state == UsbPdState_USB_PD_STATE_CONTRACT && p.has_contract_voltage) {
            rows.push_back(field("Contract", std::format("{:.2f} V {:.2f} A", p.contract_voltage,
                                                         p.has_contract_current ? p.contract_current : 0.0f)));
        }
        std::string input = p.has_input_current_limit ? std::format("{:.2f} A", p.input_current_limit) : "--";
        if (p.has_vbus_voltage) input += std::format(" · VBUS {:.2f} V", p.vbus_voltage);
        rows.push_back(field("USB in", input));
        const bool blocked = p.source_state == UsbPdSourceState_USB_PD_SOURCE_BLOCKED_DOCK ||
                             p.source_state == UsbPdSourceState_USB_PD_SOURCE_BLOCKED_BATTERY ||
                             p.source_state == UsbPdSourceState_USB_PD_SOURCE_BLOCKED_VOUT;
        std::string source = usb_pd_source_state_name(p.source_state);
        if (p.has_source_path && p.source_path != UsbPdSourcePath_USB_PD_SOURCE_PATH_NONE) {
            source += std::format(" · {}", usb_pd_source_path_name(p.source_path));
        }
        rows.push_back(hbox({text(" Source") | size(WIDTH, EQUAL, 10) | dim,
                             text(source) |
                                 (p.source_state == UsbPdSourceState_USB_PD_SOURCE_ACTIVE ? color(Color::Green)
                                  : blocked                                               ? color(Color::Yellow)
                                                                                          : dim)}));
        rows.push_back(field("Rd", std::format("ext {} · {} hard resets",
                                               p.has_dead_battery_rd ? (p.dead_battery_rd ? "on" : "off") : "?",
                                               p.has_hard_resets ? p.hard_resets : 0)) |
                       (p.has_dead_battery_rd && p.dead_battery_rd ? color(Color::Yellow) : nothing));
        if (p.has_capability_mismatch && p.capability_mismatch) {
            rows.push_back(row("capability mismatch") | color(Color::Yellow));
        }
        if (p.source_pdos_count) {
            rows.push_back(row("Partner source caps") | dim);
            for (pb_size_t i = 0; i < p.source_pdos_count; ++i) {
                const bool selected = p.has_selected_pdo && p.selected_pdo == i + 1u;
                Element e = text(std::format(" {} {} {}", selected ? "▶" : " ", i + 1, describe_pdo(p.source_pdos[i])));
                rows.push_back(selected ? e | bold | color(Color::Green) : e);
            }
        }
    } else {
        rows.push_back(row("no USB-C status") | dim);
        rows.push_back(row("(board without UCPD?)") | dim);
    }

    // Inputs: which one the BQ25792 uses and what limits it, whatever the USB-C state
    rows.push_back(separatorLight());
    if (ss.has_charger) {
        const auto &c = ss.charger;
        if (c.has_status_flags) {
            const bool dock = charger_inputs(c.status_flags).dock;
            std::string line = dock ? "present" : "absent";
            if (ss.has_charger_config && ss.charger_config.has_dock_input_current_limit) {
                line += std::format(" · limit {:.2f} A", ss.charger_config.dock_input_current_limit);
            }
            rows.push_back(field("Dock", line) | (dock ? nothing : dim));
        }
        rows.push_back(field("Input", input_source_name(c.input_source)));
    }
    if (ss.has_charger_config) {
        const auto &cfg = ss.charger_config;
        rows.push_back(field("In use", std::format("{:.2f} A · {:.2f} V", cfg.input_current_limit, cfg.input_voltage_limit)));
        if (cfg.has_input_current_cap || cfg.has_input_voltage_floor) {
            rows.push_back(field("Host", std::format("cap {} · floor {}",
                                                     cfg.has_input_current_cap ? std::format("{:.2f} A", cfg.input_current_cap) : "--",
                                                     cfg.has_input_voltage_floor ? std::format("{:.2f} V", cfg.input_voltage_floor) : "--")) |
                           dim);
        }
    }
    // yframe: on short terminals the panel is clipped at the bottom instead of overdrawn
    return window(text(" USB-C / Inputs ") | bold, vbox(std::move(rows)) | yframe);
}

Element Tui::scope_link_panel(const Connector::StateSnapshot &state) const {
    Elements rows;
    if (state.has_scope_status) {
        const auto &st = state.scope_status;
        rows.push_back(row(std::string("State: ") + scope_state_name(st.state)));
        if (st.state == ScopeState_SCOPE_STATE_DONE) {
            rows.push_back(row(std::format("{} ch @ {} Hz", st.num_channels, st.sample_rate_hz)));
            rows.push_back(row(std::format("Samples {}..{}", st.first_sample, st.post_trigger_samples)));
            rows.push_back(row(std::string("Trigger: ") + trigger_kind_name(st.trigger_source)));
        }
        if (st.has_error) {
            rows.push_back(row(get_error_message(static_cast<ControllerError>(st.error))) | color(Color::Red));
        }
    } else {
        rows.push_back(row("State: ? ('scope status')") | dim);
    }
    rows.push_back(separatorLight());
    rows.push_back(row("Aux: " + (connector.aux_open() ? std::string("open") : std::string("closed")) +
                       (state.aux_baudrate ? std::format(" {} Bd", state.aux_baudrate) : "")));
    const bool errors = state.link_stats.crc_errors || state.link_stats.header_errors;
    rows.push_back(row(std::format("Link {} frames, {} err", state.link_stats.frames,
                                   state.link_stats.crc_errors + state.link_stats.header_errors)) |
                   (errors ? color(Color::Yellow) : dim));
    return panel("Scope / Link", std::move(rows));
}

// =============================================================================
// Trend charts
// =============================================================================

Element Tui::trend_chart(const std::vector<const TrendSeries *> &series, int window_s) const {
    const auto now = TrendSeries::clock::now();
    const auto since = now - std::chrono::seconds(window_s);
    float lo = 0, hi = 0;
    bool any = false;
    for (const auto *s : series) {
        float a, b;
        if (s->range(since, a, b)) {
            lo = any ? std::min(lo, a) : a;
            hi = any ? std::max(hi, b) : b;
            any = true;
        }
    }
    if (hi - lo < 0.05f) {   // flat signal: give it some room
        lo -= 0.05f;
        hi += 0.05f;
    }
    const float pad = (hi - lo) * 0.08f;
    lo -= pad;
    hi += pad;

    Elements title;
    for (size_t i = 0; i < series.size(); ++i) {
        const auto *s = series[i];
        const std::string value = s->points.empty() ? "--" : std::format("{:.2f}", s->points.back().v);
        title.push_back(text(std::format(" {} {} {} ", s->name, value, s->unit)) | color(kSeriesColors[i % 4]) | bold);
    }

    // Copy the points so the canvas callback (run at draw time) owns its data.
    std::vector<std::vector<TrendSeries::Point>> data;
    for (const auto *s : series) {
        data.emplace_back(s->points.begin(), s->points.end());
    }
    auto plot = canvas([data = std::move(data), lo, hi, since, window_s, any](Canvas &c) {
        const int w = c.width();
        const int h = c.height();
        if (!any) {
            c.DrawText(0, h / 2, "no data - status polling off? ('poll 1')");
            return;
        }
        auto y_of = [&](float v) { return static_cast<int>(std::lround((h - 1) - (v - lo) / (hi - lo) * (h - 1))); };
        for (int q = 1; q < 4; ++q) {   // faint grid at 25/50/75 %
            const int y = q * (h - 1) / 4;
            for (int x = 0; x < w; x += 8) c.DrawPoint(x, y, true, Color::GrayDark);
        }
        for (size_t i = 0; i < data.size(); ++i) {
            int px = -1, py = 0;
            for (const auto &p : data[i]) {
                if (p.t < since) continue;
                const double frac = std::chrono::duration<double>(p.t - since).count() / window_s;
                const int x = static_cast<int>(std::lround(frac * (w - 1)));
                const int y = y_of(p.v);
                if (px >= 0) {
                    c.DrawPointLine(px, py, x, y, kSeriesColors[i % 4]);
                } else {
                    c.DrawPoint(x, y, true, kSeriesColors[i % 4]);
                }
                px = x;
                py = y;
            }
        }
    });
    auto axis = hbox({text(std::format("{:.2f}", lo)) | dim, filler(), text(std::format("-{} s … now", window_s)) | dim,
                      filler(), text(std::format("{:.2f}", hi)) | dim});
    return window(hbox(std::move(title)), vbox({plot | flex, axis}));
}

// =============================================================================
// Views
// =============================================================================

Element Tui::render_overview(const Connector::StateSnapshot &state) {
    std::array<TrendSeries, kTrendCount> t;
    std::vector<DeviceLogLine> recent;
    {
        std::lock_guard lock(data_mutex);
        t = trends;
        const size_t n = std::min<size_t>(device_log.size(), 200 + log_scroll);
        const size_t end = device_log.size() - std::min(log_scroll, device_log.size());
        for (size_t i = device_log.size() - n; i < end; ++i) recent.push_back(device_log[i]);
    }
    auto panels = hbox({charger_panel(state) | flex, output_panel(state) | flex, scope_link_panel(state) | flex});
    auto charts = hbox({trend_chart({&t[kVbus], &t[kVac1], &t[kVac2]}, 60) | flex,
                        trend_chart({&t[kVbat]}, 60) | flex, trend_chart({&t[kIbus], &t[kIbat]}, 60) | flex}) |
                  size(HEIGHT, EQUAL, 9);
    Elements lines;
    for (const auto &l : recent) {
        lines.push_back(text(std::format(" {:>9.3f} {:<5} {}", l.tick_ms / 1000.0, device_log_level_name(l.level), l.text)) |
                        color(level_color(l.level)));
    }
    if (lines.empty()) lines.push_back(text(" no device log yet ('log info')") | dim);
    lines.back() = lines.back() | focus;
    auto log = window(text(" Device log (F4 for filters) ") | bold, vbox(std::move(lines)) | yframe | flex);
    return vbox({panels, charts, log | flex});
}

Element Tui::render_power(const Connector::StateSnapshot &state) {
    std::array<TrendSeries, kTrendCount> t;
    {
        std::lock_guard lock(data_mutex);
        t = trends;
    }
    const int window_s = kTrendWindowsS[trend_window];
    const std::string window_name = window_s >= 60 ? std::format("{} min", window_s / 60) : std::format("{} s", window_s);
    auto grid = vbox({
        hbox({trend_chart({&t[kVbus], &t[kVac1], &t[kVac2]}, window_s) | flex, trend_chart({&t[kIbus]}, window_s) | flex}) | flex,
        hbox({trend_chart({&t[kVbat]}, window_s) | flex, trend_chart({&t[kIbat]}, window_s) | flex}) | flex,
        trend_chart({&t[kVsys]}, window_s) | flex,
    });
    // Right column: inputs / USB-C first (it changes), the output regulator below it
    auto side = vbox({usb_pd_panel(state) | flex_grow | flex_shrink, output_panel(state)}) | size(WIDTH, EQUAL, 40);
    auto header = hbox({text(" Window: " + window_name) | bold, text("  (PgUp/PgDn)") | dim, filler(),
                        text("usb edit · charger edit · out edit · poll <s> ") | dim});
    return vbox({header, hbox({grid | flex, side}) | flex});
}

Element Tui::scope_plot(std::shared_ptr<const ScopeClient::Capture> cap) {
    const uint64_t total = cap->count;
    const uint64_t length = scope_view_length ? std::min<uint64_t>(scope_view_length, total) : total;
    const uint64_t offset = static_cast<uint64_t>(std::clamp<int64_t>(scope_view_offset, 0, static_cast<int64_t>(total - length)));
    std::vector<int> channels;
    for (size_t ch = 0; ch < cap->samples.size(); ++ch) {
        if (cap->channel_mask & (1u << ch)) channels.push_back(static_cast<int>(ch));
    }

    // Vertical range: fixed ADC range or the visible data
    int lo = 0, hi = 4095;
    if (!scope_y_full) {
        lo = 4095;
        hi = 0;
        for (int ch : channels) {
            for (uint64_t i = offset; i < offset + length; ++i) {
                if (cap->have[ch][i]) {
                    lo = std::min<int>(lo, cap->samples[ch][i]);
                    hi = std::max<int>(hi, cap->samples[ch][i]);
                }
            }
        }
        if (lo > hi) { lo = 0; hi = 4095; }
        const int pad = std::max(4, (hi - lo) / 16);
        lo = std::max(0, lo - pad);
        hi = std::min(4095, hi + pad);
        if (hi <= lo) hi = lo + 1;
    }

    auto plot = canvas([cap, channels, offset, length, lo, hi](Canvas &c) {
        const int w = c.width();
        const int h = c.height();
        auto y_of = [&](int v) { return static_cast<int>(std::lround((h - 1) - double(v - lo) / (hi - lo) * (h - 1))); };
        // Grid: 8 vertical divisions, 4 horizontal
        for (int d = 0; d <= 8; ++d) {
            const int x = d * (w - 1) / 8;
            for (int y = 0; y < h; y += 3) c.DrawPoint(x, y, true, Color::GrayDark);
        }
        for (int d = 0; d <= 4; ++d) {
            const int y = d * (h - 1) / 4;
            for (int x = 0; x < w; x += 3) c.DrawPoint(x, y, true, Color::GrayDark);
        }
        // Trigger (sample 0)
        const int64_t trig = -cap->start;
        if (trig >= static_cast<int64_t>(offset) && trig < static_cast<int64_t>(offset + length)) {
            const int x = static_cast<int>((trig - static_cast<int64_t>(offset)) * (w - 1) / std::max<uint64_t>(1, length - 1));
            for (int y = 0; y < h; y += 2) c.DrawPoint(x, y, true, Color::Red);
        }
        for (size_t k = 0; k < channels.size(); ++k) {
            const int ch = channels[k];
            const Color col = kSeriesColors[k % 4];
            const auto &s = cap->samples[ch];
            const auto &have = cap->have[ch];
            if (length <= static_cast<uint64_t>(w)) {
                // Zoomed in: connect individual samples
                int px = -1, py = 0;
                for (uint64_t i = 0; i < length; ++i) {
                    if (!have[offset + i]) { px = -1; continue; }
                    const int x = static_cast<int>(i * (w - 1) / std::max<uint64_t>(1, length - 1));
                    const int y = y_of(s[offset + i]);
                    if (px >= 0) c.DrawPointLine(px, py, x, y, col); else c.DrawPoint(x, y, true, col);
                    px = x;
                    py = y;
                }
            } else {
                // Min/max envelope per pixel column, joined to the neighbour column
                int prev_lo = -1, prev_hi = -1;
                for (int x = 0; x < w; ++x) {
                    const uint64_t a = offset + static_cast<uint64_t>(x) * length / w;
                    const uint64_t b = std::max(a + 1, offset + static_cast<uint64_t>(x + 1) * length / w);
                    int mn = 4096, mx = -1;
                    for (uint64_t i = a; i < b; ++i) {
                        if (have[i]) {
                            mn = std::min<int>(mn, s[i]);
                            mx = std::max<int>(mx, s[i]);
                        }
                    }
                    if (mx < 0) { prev_lo = -1; continue; }
                    int y_lo = y_of(mn), y_hi = y_of(mx);
                    if (prev_lo >= 0) {   // close gaps between columns
                        y_hi = std::min(y_hi, prev_lo);
                        y_lo = std::max(y_lo, prev_hi);
                    }
                    c.DrawPointLine(x, y_hi, x, y_lo, col);
                    prev_lo = y_of(mn);
                    prev_hi = y_of(mx);
                }
            }
        }
    });

    auto time_at = [&](uint64_t index) -> std::string {
        const int64_t n = cap->start + static_cast<int64_t>(index);
        return cap->sample_rate_hz ? format_time(static_cast<double>(n) / cap->sample_rate_hz) : std::format("#{}", n);
    };
    auto x_axis = hbox({text(time_at(offset)) | dim, filler(), text(time_at(offset + length / 2)) | dim, filler(),
                        text(time_at(offset + length - 1)) | dim});
    auto y_axis = vbox({text(std::format("{:>5}", hi)) | dim, filler(), text(std::format("{:>5}", (lo + hi) / 2)) | dim,
                        filler(), text(std::format("{:>5}", lo)) | dim});
    const std::string per_div = cap->sample_rate_hz
                                    ? format_time(static_cast<double>(length) / cap->sample_rate_hz / 8.0) + "/div"
                                    : std::format("{} samples/div", length / 8);
    return vbox({hbox({y_axis, separatorLight(), plot | flex}) | flex, hbox({text("      "), x_axis | flex}),
                 hbox({text(std::format(" {}  view {}..{} of {}  ", per_div, offset, offset + length, total)) | dim,
                       text(scope_y_full ? "y: 0..4095" : "y: auto") | dim, filler(),
                       text("PgUp/PgDn zoom · ←/→ pan · Home all · End trigger ") | dim})});
}

Element Tui::render_scope(const Connector::StateSnapshot &state) {
    const auto cap = scope_client.last_capture();
    if (cap.get() != scope_view_capture) {   // new capture: show all of it
        scope_view_capture = cap.get();
        scope_view_offset = 0;
        scope_view_length = 0;
    }
    std::optional<ScopeConfigure> sc;
    {
        std::lock_guard lock(data_mutex);
        sc = scope_config;
    }

    // Side panel: device state, configuration, legend
    Elements side;
    if (state.has_scope_status) {
        const auto &st = state.scope_status;
        side.push_back(row(std::string("State   ") + scope_state_name(st.state)) | bold);
        if (st.state == ScopeState_SCOPE_STATE_DONE) {
            side.push_back(row(std::format("Rate    {} Hz", st.sample_rate_hz)));
            side.push_back(row(std::format("Samples {}..{}", st.first_sample, st.post_trigger_samples)));
            side.push_back(row(std::string("Trigger ") + trigger_kind_name(st.trigger_source)));
        }
    } else {
        side.push_back(row("State   ? ('scope status')") | dim);
    }
    if (sc) {
        side.push_back(separatorLight());
        side.push_back(row("Configured") | dim);
        side.push_back(row(std::format("{} Hz, {} ch", sc->sample_rate_hz, sc->channels_count)) | dim);
        side.push_back(row(std::format("pre {} post {}", sc->pre_trigger_samples, sc->post_trigger_samples)) | dim);
    }
    if (cap) {
        side.push_back(separatorLight());
        side.push_back(row(cap->complete ? "Last capture" : "Last capture (incomplete)"));
        int k = 0;
        for (size_t ch = 0; ch < cap->samples.size(); ++ch) {
            if (!(cap->channel_mask & (1u << ch))) continue;
            std::string label = std::format("■ ch{}", ch);
            if (sc && ch < sc->channels_count) {
                label += std::format("  ADC{} IN{}", sc->channels[ch].adc, sc->channels[ch].input);
            }
            side.push_back(row(label) | color(kSeriesColors[k++ % 4]));
        }
        side.push_back(row(std::format("{} samples/ch", cap->count)) | dim);
        if (!cap->error.empty()) side.push_back(row(cap->error) | color(Color::Red));
    }
    const auto progress = scope_client.progress();
    if (progress.active) {
        side.push_back(separatorLight());
        const float frac = progress.total ? static_cast<float>(progress.received) / progress.total : 0.0f;
        side.push_back(row(std::format("Reading {:.0f}%{}", frac * 100.0f,
                                       progress.round ? std::format(" (resend {})", progress.round) : "")));
        side.push_back(gauge(frac) | color(Color::Cyan));
    }
    side.push_back(filler());
    side.push_back(row("scope edit · arm · trigger") | dim);
    side.push_back(row("scope read · load <csv>") | dim);

    Element main;
    if (cap && cap->count > 0) {
        main = scope_plot(cap);
    } else {
        main = vbox({filler(), text("No capture loaded") | bold | center,
                     text("scope edit → scope arm → scope trigger → scope read") | dim | center,
                     text("or: scope load <file.csv>") | dim | center, filler()});
    }
    return hbox({window(text(" Scope ") | bold, vbox(std::move(side))) | size(WIDTH, EQUAL, 32),
                 window(text(" Capture ") | bold, main | flex) | flex});
}

Element Tui::render_log() {
    std::vector<DeviceLogLine> lines;
    size_t total = 0;
    {
        std::lock_guard lock(data_mutex);
        total = device_log.size();
        for (const auto &l : device_log) {
            if (l.level >= log_filter_level &&
                (log_filter_text.empty() || l.text.find(log_filter_text) != std::string::npos)) {
                lines.push_back(l);
            }
        }
    }
    const size_t shown_end = lines.size() - std::min(log_scroll, lines.size());
    const size_t shown_begin = shown_end > 500 ? shown_end - 500 : 0;
    Elements rows;
    for (size_t i = shown_begin; i < shown_end; ++i) {
        const auto &l = lines[i];
        rows.push_back(hbox({text(std::format(" {:>10.3f} ", l.tick_ms / 1000.0)) | dim,
                             text(std::format("{:<5} ", device_log_level_name(l.level))) | color(level_color(l.level)) | bold,
                             text(l.text) | color(level_color(l.level))}));
    }
    if (rows.empty()) {
        rows.push_back(text(total ? " no lines match the filter ('log filter off')" : " no device log yet ('log info')") | dim);
    }
    rows.back() = rows.back() | focus;
    std::string filter = std::format("≥ {}", device_log_level_name(log_filter_level));
    if (!log_filter_text.empty()) filter += " containing \"" + log_filter_text + "\"";
    auto header = hbox({text(std::format(" Device log: {} of {} lines, filter {} ", lines.size(), total, filter)) | bold,
                        filler(),
                        text(log_scroll ? std::format("scrolled {} up · End newest ", log_scroll) : "PgUp/PgDn scroll ") | dim});
    return window(header, vbox(std::move(rows)) | yframe | flex);
}

Element Tui::render_device(const Connector::StateSnapshot &state) {
    std::optional<DeviceInfo> info;
    {
        std::lock_guard lock(data_mutex);
        info = device_info;
    }
    auto links = window(text(" Links ") | bold, vbox({
        row(std::format("Control: {} frames, {} CRC errors, {} header errors, {} bytes dropped",
                        state.link_stats.frames, state.link_stats.crc_errors, state.link_stats.header_errors,
                        state.link_stats.dropped_bytes)),
        row(std::format("Aux:     {}{}, {} frames, {} CRC errors", connector.aux_open() ? "open" : "closed",
                        state.aux_baudrate ? std::format(" (device {} Bd)", state.aux_baudrate) : "",
                        state.aux_stats.frames, state.aux_stats.crc_errors)),
    }));
    if (!info) {
        return vbox({text(" Requesting device info...") | dim, links, filler()});
    }
    const auto &d = *info;
    const std::string reset = reset_cause(d.reset_flags);
    const bool bad_reset = reset.find("WDG") != std::string::npos || reset.find("BOR") != std::string::npos;
    auto general = window(text(" Device ") | bold, vbox({
        row("Board     " + std::string(d.board)),
        row("Firmware  " + std::string(d.firmware)),
        row(std::format("Uptime    {:.1f} s", d.uptime_ms / 1000.0)),
        row("Reset     " + reset) | (bad_reset ? color(Color::Red) : nothing),
        row(std::format("Heap      {} B free, minimum {} B", d.heap_free, d.heap_min_free)) |
            (d.heap_min_free < 1024 ? color(Color::Red) : nothing),
        row(std::format("Log       {} messages dropped", d.log_dropped)) | (d.log_dropped ? color(Color::Yellow) : nothing),
        row(std::string("PSRAM     ") + (d.psram_ok ? "OK" : "FAILED")) | (d.psram_ok ? nothing : color(Color::Red)),
        d.has_psram_config ? paragraph(std::string("          ") + d.psram_config) | dim : text(""),
    }));

    static constexpr const char *kStates[] = {"running", "ready", "blocked", "suspended", "deleted"};
    std::vector<Elements> table;
    table.push_back({text(" Thread ") | bold, text(" Prio ") | bold, text(" State ") | bold, text(" Stack free ") | bold});
    uint32_t max_free = 1;
    for (pb_size_t i = 0; i < d.threads_count; ++i) max_free = std::max(max_free, d.threads[i].stack_free_bytes);
    for (pb_size_t i = 0; i < d.threads_count; ++i) {
        const auto &t = d.threads[i];
        const Color c = t.stack_free_bytes < 128 ? Color::Red : t.stack_free_bytes < 256 ? Color::Yellow : Color::Green;
        table.push_back({text(" " + std::string(t.name) + " "), text(std::format(" {:>3} ", t.priority)),
                         text(std::string(" ") + (t.state < 5 ? kStates[t.state] : "?") + " "),
                         hbox({text(std::format(" {:>5} B ", t.stack_free_bytes)) | color(c),
                               gauge(static_cast<float>(t.stack_free_bytes) / max_free) | color(c) | size(WIDTH, EQUAL, 16)})});
    }
    auto threads = window(text(" Threads (stack high-water mark) ") | bold, gridbox(std::move(table)));
    return vbox({hbox({general | flex, threads | flex}), links, filler(),
                 text(" Refreshed every 5 s while this view is open ('info' to refresh now)") | dim});
}

Element Tui::render_form() const {
    const Form &f = *form;
    Elements rows;
    for (size_t i = 0; i < f.fields.size(); ++i) {
        const auto &field = f.fields[i];
        if (field.key.empty()) {
            rows.push_back(separatorLight());
            rows.push_back(text(" " + field.label) | dim);
            continue;
        }
        const bool focused = i == f.focus;
        Element value = text(" " + field.value + (focused ? "▏" : " ")) | size(WIDTH, EQUAL, 22);
        value = focused ? value | inverted : value | underlined;
        rows.push_back(hbox({text(" " + field.label) | size(WIDTH, EQUAL, 28), value, text("  " + field.help) | dim}));
    }
    return window(text(" " + f.title + " ") | bold, vbox(std::move(rows))) | size(WIDTH, GREATER_THAN, 70);
}

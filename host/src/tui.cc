#include "tui.hpp"
#include "cli.hpp"
#include "connector.hpp"
#include "error_host.hpp"
#include "periodic_task.hpp"
#include "tasks.hpp"
#include <boost/asio/detail/epoll_reactor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/log/trivial.hpp>
#include <format>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <boost/asio/post.hpp>
#include <charconv>
#include <optional>
#include <type_traits>

using namespace std::chrono_literals;

// Signal handler for FTXUI exit - calls screen.Exit() directly
static void sigint_handler(int) {
    auto* active_screen = ftxui::ScreenInteractive::Active();
    if (active_screen) {
        active_screen->Exit();
    }
}

Tui::Tui(const Options &cfg) :
    io_ctx(),
    heartbeat_timer(io_ctx),
    connector(io_ctx, std::bind(&Tui::refresh, this), std::bind(&Tui::add_log, this, std::placeholders::_1)),
    scope_client(io_ctx, [this](const Request& req) { return connector.send(req); },
                 [this](const std::string& line) { add_log(line); }),
    screen(ftxui::ScreenInteractive::Fullscreen())
{
    connector.set_message_hook([this](const Response& resp, frame::Type type, uint16_t seq) {
        on_message(resp, type, seq);
    });
    connector.set_data_hook([this](const frame::View& view) { scope_client.on_data(view); });

    BOOST_LOG_TRIVIAL(info) << "Connecting...\n";
    if (!connector.open(cfg, cfg.use_emulator)) {
        BOOST_LOG_TRIVIAL(error) << "Failed to open serial port\n";
        throw std::runtime_error("failed to open serial port");
    }

    add_log("System initialized");

    if (cfg.keepalive) {
        schedule_background_task(
            BackGroundTasks::Keepalive,
            std::chrono::seconds(*cfg.keepalive),
            &keepalive_task
        );
    }

    if (cfg.status_poll && *cfg.status_poll > 0) {
        schedule_background_task(
            BackGroundTasks::StatusPoll,
            std::chrono::seconds(*cfg.status_poll),
            &status_poll_task
        );
    }

    connector.async_run_receiver();
    status_poll_task(connector);   // initial state

    // Install SIGINT handler for Ctrl-C support
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
}

void Tui::focus_next() noexcept {
    switch (focused) {
        case FocusedComponent::Status:
            focused = FocusedComponent::Log;
            break;
        case FocusedComponent::Log:
            focused = FocusedComponent::Command;
            break;
        case FocusedComponent::Command:
            focused = FocusedComponent::Status;
            break;
    }
    refresh();
}

void Tui::focus_prev() noexcept {
    switch (focused) {
        case FocusedComponent::Status:
            focused = FocusedComponent::Command;
            break;
        case FocusedComponent::Log:
            focused = FocusedComponent::Status;
            break;
        case FocusedComponent::Command:
            focused = FocusedComponent::Log;
            break;
    }
    refresh();
}

void Tui::refresh() noexcept {
    if (!running.load()) {
        return;
    }
    if (refresh_pending.exchange(true)) {
        return;
    }
    BOOST_LOG_TRIVIAL(debug) << "Tui::refresh() called - posting Custom event";
    screen.PostEvent(ftxui::Event::Custom);
}

void Tui::run() noexcept {
    auto root = renderer();

    auto event_handler = ftxui::CatchEvent(root, [this](ftxui::Event event) {
        // Handle Custom event (triggered by refresh())
        if (event == ftxui::Event::Custom) {
            refresh_pending.store(false);
            BOOST_LOG_TRIVIAL(debug) << "Tui::event_handler() - Custom event handled, returning true";
            return true;
        }
        
        // Handle command input
        if (command_mode) {
            if (event == ftxui::Event::Backspace) {
                if (!command_input.empty()) {
                    command_input.pop_back();
                }
                return true;
            }

            if (event == ftxui::Event::Return) {
                if (!command_input.empty()) {
                    execute_command(command_input);
                    command_input.clear();
                    command_mode = false;
                }
                return true;
            }

            if (event == ftxui::Event::Escape) {
                command_input.clear();
                command_mode = false;
                return true;
            }

            if (event.is_character()) {
                command_input += event.character();
                return true;
            }
        } else {
            if (event == ftxui::Event::Escape ||
                (event.is_character() && event.character() == "q")) {
                screen.Exit();
                return true;
            }

            // Check for voltage graph toggle key 'G' or 'g' first
            if (event.is_character() && (event.character()[0] == 'G' || event.character()[0] == 'g')) {
                voltage_graph_enabled = !voltage_graph_enabled;
                refresh();
                return true;
            }

            // Start command mode on any character
            if (event.is_character()) {
                command_mode = true;
                command_input += event.character();
                return true;
            }

            // Scroll keys work in normal mode
            if (event == ftxui::Event::PageDown) {
                std::lock_guard<std::mutex> lock(log_mutex);
                if (log_scroll_offset + log_max_visible < log_buffer.size()) {
                    log_scroll_offset += log_max_visible;
                    if (log_scroll_offset + log_max_visible > log_buffer.size()) {
                        log_scroll_offset = log_buffer.size() - log_max_visible;
                    }
                }
                return true;
            }

            if (event == ftxui::Event::PageUp) {
                std::lock_guard<std::mutex> lock(log_mutex);
                if (log_scroll_offset > 0) {
                    log_scroll_offset -= log_max_visible;
                    if (log_scroll_offset > 0 && log_scroll_offset < log_max_visible) {
                        log_scroll_offset = 0;
                    }
                }
                return true;
            }

            if (event == ftxui::Event::Home) {
                std::lock_guard<std::mutex> lock(log_mutex);
                log_scroll_offset = 0;
                return true;
            }

            if (event == ftxui::Event::End) {
                std::lock_guard<std::mutex> lock(log_mutex);
                if (log_buffer.size() > log_max_visible) {
                    log_scroll_offset = log_buffer.size() - log_max_visible;
                } else {
                    log_scroll_offset = 0;
                }
                return true;
            }

            if (event == ftxui::Event::ArrowDown) {
                if (focused == FocusedComponent::Log) {
                    std::lock_guard<std::mutex> lock(log_mutex);
                    if (log_scroll_offset + log_max_visible < log_buffer.size()) {
                        log_scroll_offset++;
                    }
                } else {
                    focus_next();
                }
                return true;
            }

            if (event == ftxui::Event::ArrowUp) {
                if (focused == FocusedComponent::Log) {
                    std::lock_guard<std::mutex> lock(log_mutex);
                    if (log_scroll_offset > 0) {
                        log_scroll_offset--;
                    }
                } else {
                    focus_prev();
                }
                return true;
            }
        }

        return false;
    });

    ftxui::Loop loop(&screen, event_handler);
    running.store(true);
    schedule_heartbeat_refresh();
    std::thread io_thread([this] {
        io_ctx.run();
        BOOST_LOG_TRIVIAL(info) << "io service exited\n";
    });

    // FTXUI Loop with SIGINT handler that calls screen.Exit()
    // The signal handler (installed in constructor) will call active_screen->Exit()
    // when SIGINT is received, causing HasQuitted() to return true
    loop.Run();
    running.store(false);
    io_ctx.stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    heartbeat_timer.cancel();
    connector.close();
}

void Tui::schedule_heartbeat_refresh() {
    heartbeat_timer.expires_after(std::chrono::seconds(1));
    heartbeat_timer.async_wait([this](boost::system::error_code ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << "TUI heartbeat timer failed: " << ec.message();
            return;
        }

        last_heartbeat.store(std::time(nullptr));
        refresh();
        schedule_heartbeat_refresh();
    });
}

void Tui::add_log(const std::string &line) noexcept {
    std::lock_guard<std::mutex> lock(log_mutex);
    BOOST_LOG_TRIVIAL(debug) << "Tui::add_log() called with: " << line;
    log_buffer.push_back(line);

    // Reset scroll offset to show newest entries
    if (log_scroll_offset == 0 || log_buffer.size() <= log_max_visible) {
        log_scroll_offset = 0;
    } else {
        size_t max_offset = log_buffer.size() > log_max_visible ? log_buffer.size() - log_max_visible : 0;
        if (log_scroll_offset > max_offset) {
            log_scroll_offset = max_offset;
        }
    }

    if (log_buffer.size() > 100) {
        log_buffer.pop_front();
        size_t max_offset = log_buffer.size() > log_max_visible ? log_buffer.size() - log_max_visible : 0;
        if (log_scroll_offset > max_offset) {
            log_scroll_offset = max_offset;
        }
    }

    BOOST_LOG_TRIVIAL(debug) << "Tui::add_log() calling refresh(), buffer size: " << log_buffer.size();
    refresh();
}

namespace {

const char* charge_status_name(ChargeStatus s) {
    switch (s) {
        case ChargeStatus_CHARGE_STATUS_NOT_CHARGING:   return "Not charging";
        case ChargeStatus_CHARGE_STATUS_TRICKLE_CHARGE: return "Trickle";
        case ChargeStatus_CHARGE_STATUS_PRECHARGE:      return "Precharge";
        case ChargeStatus_CHARGE_STATUS_FAST_CHARGE:    return "Fast (CC)";
        case ChargeStatus_CHARGE_STATUS_TAPER_CHARGE:   return "Taper (CV)";
        case ChargeStatus_CHARGE_STATUS_TOP_OFF:        return "Top off";
        case ChargeStatus_CHARGE_STATUS_CHARGE_DONE:    return "Done";
        default:                                        return "Unknown";
    }
}

const char* input_source_name(ChargerInputSource s) {
    switch (s) {
        case ChargerInputSource_CHARGER_INPUT_USB_DPDM: return "USB D+/D-";
        case ChargerInputSource_CHARGER_INPUT_AC1:      return "VAC1 (USB-C)";
        case ChargerInputSource_CHARGER_INPUT_AC2:      return "VAC2 (dock)";
        case ChargerInputSource_CHARGER_INPUT_AUTO:     return "Auto";
        default:                                        return "?";
    }
}

const char* vbus_status_name(VBUSStatus s) {
    switch (s) {
        case VBUSStatus_VBUS_STATUS_NO_INPUT:              return "No input";
        case VBUSStatus_VBUS_STATUS_USB_SDP_500MA:         return "USB SDP";
        case VBUSStatus_VBUS_STATUS_USB_CDP_1500MA:        return "USB CDP";
        case VBUSStatus_VBUS_STATUS_USB_DCP_3250MA:        return "USB DCP";
        case VBUSStatus_VBUS_STATUS_ADJUSTABLE_HIGH:       return "HV adapter";
        case VBUSStatus_VBUS_STATUS_UNKNOWN_ADAPTER:       return "Unknown adapter";
        case VBUSStatus_VBUS_STATUS_NON_STANDARD_ADAPTER:  return "Non-std adapter";
        case VBUSStatus_VBUS_STATUS_OTG_MODE:              return "OTG";
        case VBUSStatus_VBUS_STATUS_NOT_QUALIFIED_ADAPTER: return "Not qualified";
        case VBUSStatus_VBUS_STATUS_POWERED_FROM_VBUS:     return "Powered from VBUS";
        default:                                           return "?";
    }
}

const char* regulator_mode_name(OutputRegulatorOperatingMode m) {
    switch (m) {
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK:            return "Buck";
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BOOST:           return "Boost";
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK_BOOST:      return "Buck-Boost";
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_FREQUENCY_LIMIT: return "Freq limit";
        default:                                                                           return "?";
    }
}

const char* scope_state_name(ScopeState s) {
    switch (s) {
        case ScopeState_SCOPE_STATE_IDLE:        return "Idle";
        case ScopeState_SCOPE_STATE_PRETRIGGER:  return "Armed";
        case ScopeState_SCOPE_STATE_POSTTRIGGER: return "Triggered";
        case ScopeState_SCOPE_STATE_STOPPING:    return "Stopping";
        case ScopeState_SCOPE_STATE_DONE:        return "Done";
        case ScopeState_SCOPE_STATE_ERROR:       return "Error";
        default:                                 return "?";
    }
}

const char* trigger_kind_name(ScopeTriggerKind k) {
    switch (k) {
        case ScopeTriggerKind_SCOPE_TRIGGER_SOFTWARE:        return "software";
        case ScopeTriggerKind_SCOPE_TRIGGER_ANALOG_WATCHDOG: return "AWD";
        case ScopeTriggerKind_SCOPE_TRIGGER_COMPARATOR:      return "COMP";
        case ScopeTriggerKind_SCOPE_TRIGGER_EXTERNAL:        return "external";
        default:                                             return "?";
    }
}

ftxui::Element panel(const std::string& title, std::vector<ftxui::Element> rows) {
    rows.insert(rows.begin(), ftxui::text(" " + title + " ") | ftxui::bold);
    return ftxui::vbox(std::move(rows)) | ftxui::border;
}

ftxui::Element row(const std::string& s) {
    return ftxui::text(" " + s);
}

} // namespace

ftxui::Component Tui::render_status() const noexcept {
    return ftxui::Renderer([this] {
        const auto now = std::time(nullptr);
        const auto state = connector.get_state();
        const SystemStatus& ss = state.system_status;

        std::string status_text = command_mode ? "Command mode | Enter to run | Esc to cancel"
                                               : "Type to enter a command ('help') | q quits";
        status_text += " | Ping: " + (state.last_ping ? std::format("{}s ago", now - state.last_ping) : std::string("never"));
        status_text += " | Status: " + (state.last_status ? std::format("{}s ago", now - state.last_status) : std::string("never"));
        status_text += now - last_heartbeat.load() == 0 ? " ●" : " ○";

        // Charger
        std::vector<ftxui::Element> charger;
        if (ss.has_charger) {
            const auto& c = ss.charger;
            charger.push_back(row(std::string(c.enabled ? "[CHG ON]" : "[CHG OFF]") + (c.fault_present ? " FAULT" : "") +
                                  (c.battery_present ? "" : " NO BAT")));
            charger.push_back(row(std::string("State: ") + charge_status_name(c.charge_status)));
            charger.push_back(row(std::string("Input: ") + input_source_name(c.input_source)));
            charger.push_back(row(std::string("VBUS: ") + vbus_status_name(c.vbus_status)));
        } else {
            charger.push_back(row("[?] no status"));
        }
        if (ss.has_charger_measurements) {
            const auto& m = ss.charger_measurements;
            charger.push_back(row(std::format("VBUS {:5.2f}V IBUS {:5.2f}A", m.vbus_voltage, m.ibus_current)));
            charger.push_back(row(std::format("VBAT {:5.2f}V IBAT {:5.2f}A", m.vbat_voltage,
                                              m.has_ibat_current ? m.ibat_current : 0.0f)));
            charger.push_back(row(std::format("VSYS {:5.2f}V", m.vsys_voltage)));
            if (m.has_vac1_voltage || m.has_vac2_voltage) {
                charger.push_back(row(std::format("VAC1 {:5.2f}V VAC2 {:5.2f}V", m.vac1_voltage, m.vac2_voltage)));
            }
        }
        if (ss.has_charger_config) {
            const auto& cfg = ss.charger_config;
            charger.push_back(row(std::format("Lim chg {:.2f}V {:.2f}A", cfg.charge_voltage_limit, cfg.charge_current_limit)));
            charger.push_back(row(std::format("Lim in  {:.2f}V {:.2f}A", cfg.input_voltage_limit, cfg.input_current_limit)));
        }

        // Output regulator
        std::vector<ftxui::Element> output;
        if (ss.has_output_regulator) {
            const auto& o = ss.output_regulator;
            output.push_back(row(std::string(o.enabled ? "[OUT ON]" : "[OUT OFF]") + (o.scp_fault ? " SCP" : "") +
                                 (o.ocp_fault ? " OCP" : "") + (o.ovp_fault ? " OVP" : "")));
            output.push_back(row(std::string("Mode: ") + regulator_mode_name(o.operating_mode) +
                                 (o.feedback_mode == OutputRegulatorFeedbackMode_OUTPUT_REGULATOR_FEEDBACK_MODE_PFM ? " PFM" : " PWM")));
        } else {
            output.push_back(row("[?] no status"));
        }
        if (ss.has_output_regulator_config) {
            const auto& cfg = ss.output_regulator_config;
            output.push_back(row(std::format("Set  {:5.2f}V", cfg.output_voltage)));
            output.push_back(row(std::format("Ilim {:5.2f}A", cfg.output_current_limit)));
            output.push_back(row(std::format("Rsns {:.3f}R", cfg.sense_resistor_ohms)));
        } else if (ss.has_output_regulator_measurements) {
            // The TPS55289 has no ADC: these are its setpoints, not measurements.
            const auto& m = ss.output_regulator_measurements;
            output.push_back(row(std::format("Set  {:5.2f}V", m.output_voltage)));
            output.push_back(row(std::format("Ilim {:5.2f}A", m.output_current)));
        }

        // Oscilloscope + data link
        std::vector<ftxui::Element> scope;
        if (state.has_scope_status) {
            const auto& st = state.scope_status;
            scope.push_back(row(std::string("State: ") + scope_state_name(st.state)));
            if (st.state == ScopeState_SCOPE_STATE_DONE) {
                scope.push_back(row(std::format("{} ch @ {} Hz", st.num_channels, st.sample_rate_hz)));
                scope.push_back(row(std::format("Samples {}..{}", st.first_sample, st.post_trigger_samples)));
                scope.push_back(row(std::string("Trigger: ") + trigger_kind_name(st.trigger_source)));
            }
            if (st.has_error) {
                scope.push_back(row(get_error_message(static_cast<ControllerError>(st.error))));
            }
        } else {
            scope.push_back(row("State: ? ('scope status')"));
        }
        const auto progress = scope_client.progress();
        if (progress.active) {
            const double pct = progress.total ? 100.0 * progress.received / progress.total : 0.0;
            scope.push_back(row(std::format("Read {:.0f}%{}", pct,
                                            progress.round ? std::format(" (resend {})", progress.round) : "")));
        }
        scope.push_back(row("Aux: " + (connector.aux_open() ? std::string("open") : std::string("closed")) +
                            (state.aux_baudrate ? std::format(" {} Bd", state.aux_baudrate) : "")));
        if (state.link_stats.crc_errors || state.link_stats.header_errors) {
            scope.push_back(row(std::format("Link err crc {} hdr {}", state.link_stats.crc_errors,
                                            state.link_stats.header_errors)));
        }

        auto panels = ftxui::hbox({
            panel("Charger", std::move(charger)) | ftxui::flex,
            panel("Output Regulator", std::move(output)) | ftxui::flex,
            panel("Scope / Link", std::move(scope)) | ftxui::flex,
        });
        return ftxui::vbox({panels, ftxui::text(status_text)}) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 100);
    });
}

ftxui::Component Tui::render_log() const noexcept {
    return ftxui::Renderer([this] {
        std::lock_guard<std::mutex> lock(log_mutex);
        BOOST_LOG_TRIVIAL(debug) << "Tui::render_log() called, buffer size: " << log_buffer.size() << ", scroll offset: " << log_scroll_offset;

        const size_t total_lines = log_buffer.size();
        const size_t max_scroll_offset = total_lines > log_max_visible ? total_lines - log_max_visible : 0;
        const size_t scroll_offset = std::min(log_scroll_offset, max_scroll_offset);

        std::vector<ftxui::Element> log_elements;
        if (total_lines == 0) {
            log_elements.push_back(ftxui::text("No logs yet..."));
        } else {
            const size_t newest_index = total_lines - 1 - scroll_offset;
            for (size_t count = 0; count < log_max_visible && count <= newest_index; ++count) {
                log_elements.push_back(ftxui::text(log_buffer[newest_index - count]));
            }
        }

        auto log_vbox = ftxui::vbox(log_elements) | ftxui::border;
        if (focused == FocusedComponent::Log) {
            log_vbox = log_vbox | ftxui::borderHeavy;
        }

        return ftxui::yflex_grow(log_vbox);
    }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 100);
}

ftxui::Component Tui::render_command_input() const noexcept {
    return ftxui::Renderer([this] {
        if (!command_mode) {
            auto element = ftxui::text("Type command directly");

            if (focused == FocusedComponent::Command) {
                element = element | ftxui::borderHeavy;
            }

            return element | ftxui::border;
        }

        std::string prompt = "> ";
        std::string display_input = prompt + command_input;

        auto element = ftxui::text(display_input);

        if (command_input.empty()) {
            element = element | ftxui::inverted;
        }

        element = element | ftxui::border;

        if (focused == FocusedComponent::Command) {
            element = element | ftxui::borderHeavy;
        }

        return element;
    }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 100) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 3);
}

ftxui::Component Tui::render_voltage_graph() const noexcept {
    return ftxui::Renderer([this] {
        // Sample voltage values from connector state
        // Note: This may block if emulator holds lock, but at 20Hz update rate
        // the blocking time is minimal (5ms lock every 50ms = 10% utilization)
        const auto state = connector.get_state();

        float vbat = state.system_status.charger_measurements.vbat_voltage;
        float vbus = state.system_status.charger_measurements.vbus_voltage;
        float vout = state.system_status.output_regulator_measurements.output_voltage;

        // Update sample buffers with latest voltage values
        vbat_samples.push_back(vbat);
        if (vbat_samples.size() > voltage_graph_size) {
            vbat_samples.pop_front();
        }

        vbus_samples.push_back(vbus);
        if (vbus_samples.size() > voltage_graph_size) {
            vbus_samples.pop_front();
        }

        vout_samples.push_back(vout);
        if (vout_samples.size() > voltage_graph_size) {
            vout_samples.pop_front();
        }

        // Calculate graph dimensions (leave room for axis labels)
        const int graph_width = 80;
        const int graph_height = 12;  // Reduced to make room for labels
        const int left_margin = 8;    // Space for Y-axis labels

        // Find min/max for scaling
        float min_voltage = 0.0f;
        float max_voltage = 5.0f;

        auto update_range = [&min_voltage, &max_voltage](const std::deque<float>& samples) {
            if (samples.empty()) return;
            float local_min = *std::min_element(samples.begin(), samples.end());
            float local_max = *std::max_element(samples.begin(), samples.end());
            if (local_min < min_voltage) min_voltage = local_min;
            if (local_max > max_voltage) max_voltage = local_max;
        };

        update_range(vbat_samples);
        update_range(vbus_samples);
        update_range(vout_samples);

        // Add some padding
        float range = max_voltage - min_voltage;
        if (range < 0.1f) range = 0.1f;
        min_voltage -= range * 0.1f;
        max_voltage += range * 0.1f;
        range = max_voltage - min_voltage;

        // Build graph grid
        std::vector<std::vector<wchar_t>> grid(graph_height, std::vector<wchar_t>(graph_width, L' '));

        // Draw grid lines (horizontal)
        int num_horizontal_lines = 5;
        for (int i = 0; i <= num_horizontal_lines; ++i) {
            int row = i * (graph_height - 1) / num_horizontal_lines;
            for (int x = left_margin; x < graph_width; ++x) {
                grid[row][x] = L'\u2500';  // Horizontal grid line
            }
        }

        // Draw grid lines (vertical) - every 20 columns
        for (int x = left_margin; x < graph_width; x += 20) {
            for (int row = 0; row < graph_height; ++row) {
                grid[row][x] = L'\u2502';  // Vertical grid line
            }
        }

        // Draw axes intersection
        for (int x = left_margin; x < left_margin + 4; ++x) {
            grid[graph_height - 1][x] = L'\u2500';
        }
        for (int row = 0; row < graph_height; ++row) {
            grid[row][left_margin] = L'\u2502';
        }
        // Corner
        grid[graph_height - 1][left_margin] = L'\u2514';  // Bottom-left corner

        // Draw voltage traces
        auto draw_trace = [&grid, min_voltage, range](
            const std::deque<float>& samples, wchar_t symbol) {
            if (samples.size() < 2) return;

            int prev_x = left_margin;
            float prev_voltage = samples.front();
            int prev_y = graph_height - 1 - static_cast<int>(
                (prev_voltage - min_voltage) / range * (graph_height - 1)
            );

            for (size_t i = 1; i < samples.size(); ++i) {
                int x = left_margin + static_cast<int>(i * (graph_width - left_margin - 1) / (samples.size() - 1.0f));
                float voltage = samples[i];
                int y = graph_height - 1 - static_cast<int>(
                    (voltage - min_voltage) / range * (graph_height - 1)
                );

                // Clamp to grid bounds
                if (y < 0) y = 0;
                if (y >= graph_height) y = graph_height - 1;

                // Draw line (Bresenham-like simplified)
                if (x == prev_x) {
                    // Vertical line
                    int y_start = std::min(prev_y, y);
                    int y_end = std::max(prev_y, y);
                    for (int ry = y_start; ry <= y_end; ++ry) {
                        grid[ry][x] = symbol;
                    }
                } else {
                    // Horizontal line at prev_y, then vertical at x
                    int x_start = std::min(prev_x, x);
                    int x_end = std::max(prev_x, x);
                    for (int rx = x_start; rx <= x_end && rx < graph_width; ++rx) {
                        grid[prev_y][rx] = symbol;
                    }
                }

                prev_x = x;
                prev_y = y;
            }
        };

        // Draw traces
        draw_trace(vbat_samples, L'\u2588');  // VBAT - solid block
        draw_trace(vbus_samples, L'\u2591');  // VBUS - shaded
        draw_trace(vout_samples, L'\u2502');  // VOUT - pipe

        // Build text output
        std::vector<ftxui::Element> graph_elements;

        // Y-axis labels (left side)
        std::vector<std::pair<float, int>> y_labels;
        y_labels.emplace_back(max_voltage, 0);
        y_labels.emplace_back(min_voltage + range * 0.75f, graph_height / 4);
        y_labels.emplace_back(min_voltage + range * 0.5f, graph_height / 2);
        y_labels.emplace_back(min_voltage + range * 0.25f, 3 * graph_height / 4);
        y_labels.emplace_back(min_voltage, graph_height - 1);

        for (int row = 0; row < graph_height; ++row) {
            std::wstring label_row(left_margin, L' ');
            
            // Find matching label for this row
            for (const auto& [voltage, label_row_idx] : y_labels) {
                if (row == label_row_idx) {
                    const auto label = std::format("{:5.1f}V", voltage);
                    label_row.append(label.begin(), label.end());
                    break;
                }
            }
            
            // Add grid line character at intersection
            if (row == graph_height - 1) {
                label_row[left_margin] = L'\u2514';
            } else if (row > 0 && row < graph_height - 1) {
                label_row[left_margin] = L'\u2500';
            }
            
            graph_elements.push_back(ftxui::text(label_row));
        }

        // X-axis labels (bottom)
        std::wstring bottom_row(graph_width, L' ');
        bottom_row[left_margin] = L'\u2514';
        
        // Time labels at intervals
        int time_interval = graph_width / 4;
        for (int i = 0; i <= 4; ++i) {
            int x = left_margin + i * time_interval;
            if (x < graph_width) {
                std::swprintf(&bottom_row[x], 6, L"%d00ms", i);
            }
        }
        graph_elements.push_back(ftxui::text(bottom_row));

        // Legend
        std::string legend = "VBAT: █  VBUS: ░  VOUT: │";
        graph_elements.push_back(ftxui::text(legend));

        // Title
        graph_elements.push_back(ftxui::text("Voltage Monitoring (V)"));

        auto graph_box = ftxui::vbox(graph_elements) | ftxui::border;
        if (!voltage_graph_enabled) {
            graph_box = ftxui::text("Voltage graph disabled (press 'G' to toggle)");
        }

        return graph_box | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 100);
    });
}

ftxui::Component Tui::renderer() const noexcept {
    auto status_component = render_status();
    auto log_component = render_log();
    auto command_component = render_command_input();
    auto voltage_graph_component = render_voltage_graph();

    ftxui::Components components;
    components.push_back(status_component);
    components.push_back(voltage_graph_component);
    components.push_back(log_component);
    components.push_back(command_component);

    auto root = ftxui::Container::Vertical(components);

    // Apply flex-grow to log element so it fills available vertical space
    // Status and Command have fixed heights (10 and 3 lines)
    // Log will grow to fill remaining space via yflex_grow()

    return root;
}

namespace {

std::vector<std::string> split_words(const std::string& line) {
    std::vector<std::string> words;
    std::string cur;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') {
            quoted = !quoted;
        } else if (c == ' ' && !quoted) {
            if (!cur.empty()) words.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) words.push_back(std::move(cur));
    return words;
}

template <typename T>
std::optional<T> parse_number(const std::string& s) {
    T value{};
    const char* first = s.data();
    const char* last = s.data() + s.size();
    int base = 10;
    if constexpr (std::is_integral_v<T>) {
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            first += 2;
            base = 16;
        }
        auto [ptr, ec] = std::from_chars(first, last, value, base);
        if (ec != std::errc() || ptr != last) return std::nullopt;
    } else {
        auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc() || ptr != last) return std::nullopt;
    }
    return value;
}

// "key=value" -> {key, value}; bare word -> {word, ""}
std::pair<std::string, std::string> key_value(const std::string& word) {
    const auto eq = word.find('=');
    if (eq == std::string::npos) return {word, ""};
    return {word.substr(0, eq), word.substr(eq + 1)};
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const auto pos = s.find(sep, start);
        parts.push_back(s.substr(start, pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return parts;
}

} // namespace

void Tui::on_message(const Response& resp, frame::Type type, uint16_t seq) noexcept {
    scope_client.on_message(resp, type, seq);

    switch (resp.which_payload) {
        case Response_scope_status_tag: {
            const auto& st = resp.payload.scope_status;
            std::string msg = std::string("Scope: ") + scope_state_name(st.state);
            if (st.state == ScopeState_SCOPE_STATE_DONE) {
                msg += std::format(", {} ch @ {} Hz, samples {}..{}, trigger {}", st.num_channels, st.sample_rate_hz,
                                   st.first_sample, st.post_trigger_samples, trigger_kind_name(st.trigger_source));
            }
            add_log(msg);
            break;
        }
        case Response_scope_read_started_tag: {
            const auto& r = resp.payload.scope_read_started;
            add_log(std::format("Scope: transfer {} started, {} frames on {} link", r.transfer_id, r.frames,
                                r.link == DataLink_DATA_LINK_AUX ? "aux" : "control"));
            break;
        }
        case Response_data_link_status_tag: {
            const uint32_t actual = resp.payload.data_link_status.baudrate;
            const uint32_t requested = requested_aux_baud.exchange(0);
            std::string msg = std::format("Data link: device aux UART at {} Bd", actual);
            if (requested && connector.aux_open()) {
                msg += connector.set_aux_baudrate(requested) ? std::format(", host aux port set to {} Bd", requested)
                                                             : ", host aux port baud change FAILED";
            }
            add_log(msg);
            break;
        }
        default:
            break;
    }
}

void Tui::schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period, PeriodicTask::callback_t func) noexcept {
    boost::asio::post(io_ctx, [this, type, period, func = std::move(func)]() mutable {
        tasks.erase(type);
        auto [it, inserted] = tasks.emplace(type, PeriodicTask(io_ctx, period, std::move(func), connector));
        it->second.start();   // start in place: the timer handler captures the stored task
    });
}

void Tui::stop_background_task(BackGroundTasks type) noexcept {
    boost::asio::post(io_ctx, [this, type] {
        if (auto it = tasks.find(type); it != tasks.end()) {
            it->second.stop();
            tasks.erase(it);
        }
    });
}

void Tui::execute_command(const std::string& command_line) noexcept {
    add_log("> " + command_line);
    const auto words = split_words(command_line);
    if (words.empty()) {
        return;
    }
    const std::string& cmd = words[0];
    const std::vector<std::string> args(words.begin() + 1, words.end());

    if (cmd == "help" || cmd == "h" || cmd == "?") {
        show_help(args.empty() ? "" : args[0]);
    } else if (cmd == "ping") {
        Request req = Request_init_zero;
        req.which_command = Request_ping_tag;
        connector.send(req);
    } else if (cmd == "status") {
        status_poll_task(connector);
    } else if (cmd == "poll") {
        handle_poll(args);
    } else if (cmd == "charger" || cmd == "chg") {
        handle_charger(args);
    } else if (cmd == "out" || cmd == "output") {
        handle_output(args);
    } else if (cmd == "scope") {
        handle_scope(args);
    } else if (cmd == "link") {
        handle_link(args);
    } else if (cmd == "list") {
        handle_applet_list("");
    } else if (cmd == "start") {
        handle_applet_start(command_line.substr(command_line.find("start") + 6));
    } else if (cmd == "stop") {
        handle_applet_stop(command_line.substr(command_line.find("stop") + 5));
    } else {
        // Anything else is a command for the applet named by the first word
        handle_applet_command(command_line);
    }
}

void Tui::handle_poll(const std::vector<std::string>& args) noexcept {
    if (args.size() != 1) {
        add_log("Usage: poll <seconds>|off");
        return;
    }
    if (args[0] == "off" || args[0] == "0") {
        stop_background_task(BackGroundTasks::StatusPoll);
        add_log("Status polling off");
        return;
    }
    const auto seconds = parse_number<double>(args[0]);
    if (!seconds || *seconds < 0.1) {
        add_log("poll: interval must be >= 0.1 s");
        return;
    }
    schedule_background_task(BackGroundTasks::StatusPoll,
                             std::chrono::duration_cast<PeriodicTask::duration_t>(std::chrono::duration<double>(*seconds)),
                             &status_poll_task);
    add_log(std::format("Polling status every {} s", *seconds));
}

void Tui::handle_charger(const std::vector<std::string>& args) noexcept {
    Request req = Request_init_zero;
    req.which_command = Request_set_charger_config_tag;
    auto& c = req.command.set_charger_config;

    if (args.size() == 1 && (args[0] == "on" || args[0] == "off")) {
        c.has_enabled = true;
        c.enabled = args[0] == "on";
        connector.send(req);
        return;
    }
    if (args.empty() || args[0] != "set" || args.size() < 2) {
        add_log("Usage: charger on|off | charger set [vchg=V] [ichg=A] [vin=V] [iin=A] [src=usb|ac1|ac2|auto]");
        return;
    }
    for (size_t i = 1; i < args.size(); ++i) {
        const auto [key, value] = key_value(args[i]);
        if (key == "src") {
            if (value == "usb") c.input_source = ChargerInputSource_CHARGER_INPUT_USB_DPDM;
            else if (value == "ac1") c.input_source = ChargerInputSource_CHARGER_INPUT_AC1;
            else if (value == "ac2") c.input_source = ChargerInputSource_CHARGER_INPUT_AC2;
            else if (value == "auto") c.input_source = ChargerInputSource_CHARGER_INPUT_AUTO;
            else { add_log("charger: src must be usb, ac1, ac2 or auto"); return; }
            c.has_input_source = true;
            continue;
        }
        const auto v = parse_number<float>(value);
        if (!v) { add_log("charger: bad value in '" + args[i] + "'"); return; }
        if (key == "vchg") { c.has_charge_voltage_limit = true; c.charge_voltage_limit = *v; }
        else if (key == "ichg") { c.has_charge_current_limit = true; c.charge_current_limit = *v; }
        else if (key == "vin") { c.has_input_voltage_limit = true; c.input_voltage_limit = *v; }
        else if (key == "iin") { c.has_input_current_limit = true; c.input_current_limit = *v; }
        else { add_log("charger: unknown setting '" + key + "'"); return; }
    }
    connector.send(req);
}

void Tui::handle_output(const std::vector<std::string>& args) noexcept {
    Request req = Request_init_zero;
    req.which_command = Request_set_output_regulator_config_tag;
    auto& c = req.command.set_output_regulator_config;

    if (args.size() == 1 && (args[0] == "on" || args[0] == "off")) {
        c.has_enabled = true;
        c.enabled = args[0] == "on";
        connector.send(req);
        return;
    }
    if (args.empty() || args[0] != "set" || args.size() < 2) {
        add_log("Usage: out on|off | out set [v=V] [ilim=A] [rsense=ohm]");
        return;
    }
    for (size_t i = 1; i < args.size(); ++i) {
        const auto [key, value] = key_value(args[i]);
        const auto v = parse_number<float>(value);
        if (!v) { add_log("out: bad value in '" + args[i] + "'"); return; }
        if (key == "v") { c.has_output_voltage = true; c.output_voltage = *v; }
        else if (key == "ilim") { c.has_output_current_limit = true; c.output_current_limit = *v; }
        else if (key == "rsense") { c.has_sense_resistor_ohms = true; c.sense_resistor_ohms = *v; }
        else { add_log("out: unknown setting '" + key + "'"); return; }
    }
    connector.send(req);
}

void Tui::handle_scope(const std::vector<std::string>& args) noexcept {
    const std::string sub = args.empty() ? "" : args[0];
    Request req = Request_init_zero;

    if (sub == "arm") {
        req.which_command = Request_scope_arm_tag;
    } else if (sub == "trigger" || sub == "trig") {
        req.which_command = Request_scope_force_trigger_tag;
    } else if (sub == "stop") {
        req.which_command = Request_scope_stop_tag;
    } else if (sub == "status") {
        req.which_command = Request_scope_get_status_tag;
    } else if (sub == "cancel") {
        scope_client.cancel();
        return;
    } else if (sub == "config") {
        req.which_command = Request_scope_configure_tag;
        auto& c = req.command.scope_configure;
        bool have_rate = false, have_post = false;
        for (size_t i = 1; i < args.size(); ++i) {
            const auto [key, value] = key_value(args[i]);
            if (key == "ch") {
                // ch=<adc>:<input>[:<sampling_time 0..7>]
                const auto parts = split(value, ':');
                if (c.channels_count >= 4 || parts.size() < 2 || parts.size() > 3) {
                    add_log("scope config: ch=<adc>:<input>[:<smp 0..7>], at most 4 channels");
                    return;
                }
                auto& ch = c.channels[c.channels_count];
                const auto adc = parse_number<uint32_t>(parts[0]);
                const auto input = parse_number<uint32_t>(parts[1]);
                if (!adc || !input) { add_log("scope config: bad channel '" + value + "'"); return; }
                ch.adc = *adc;
                ch.input = *input;
                if (parts.size() == 3) {
                    const auto smp = parse_number<uint32_t>(parts[2]);
                    if (!smp || *smp > 7) { add_log("scope config: sampling time index is 0..7"); return; }
                    ch.has_sampling_time = true;
                    ch.sampling_time = *smp;
                }
                ++c.channels_count;
                continue;
            }
            if (key == "trig") {
                c.has_trigger = true;
                c.trigger.has_software = true;
                c.trigger.software = false;
                for (const auto& t : split(value, ',')) {
                    if (t == "sw") { c.trigger.software = true; }
                    else if (t == "awd") { c.trigger.has_analog_watchdog = true; c.trigger.analog_watchdog = true; }
                    else if (t == "ext") { c.trigger.has_external = true; c.trigger.external = true; }
                    else if (t.starts_with("comp")) {
                        const auto n = parse_number<uint32_t>(t.substr(4));
                        if (!n || *n < 1 || *n > 7) { add_log("scope config: comp1..comp7"); return; }
                        c.trigger.has_comparator = true;
                        c.trigger.comparator = *n;
                    } else { add_log("scope config: trig=sw,awd,comp<N>,ext"); return; }
                }
                continue;
            }
            if (key == "awd") {
                // awd=<channel index>:<low>:<high>
                const auto parts = split(value, ':');
                std::optional<uint32_t> ch, lo, hi;
                if (parts.size() == 3) {
                    ch = parse_number<uint32_t>(parts[0]);
                    lo = parse_number<uint32_t>(parts[1]);
                    hi = parse_number<uint32_t>(parts[2]);
                }
                if (!ch || !lo || !hi) { add_log("scope config: awd=<ch>:<low>:<high>"); return; }
                c.has_trigger = true;
                c.trigger.has_analog_watchdog = true;
                c.trigger.analog_watchdog = true;
                c.trigger.has_awd_channel = true;
                c.trigger.awd_channel = *ch;
                c.trigger.has_awd_low = true;
                c.trigger.awd_low = *lo;
                c.trigger.has_awd_high = true;
                c.trigger.awd_high = *hi;
                continue;
            }
            if (key == "fullpre") {
                c.has_trigger = true;
                c.trigger.has_require_full_pretrigger = true;
                c.trigger.require_full_pretrigger = value != "0";
                continue;
            }
            const auto n = parse_number<uint32_t>(value);
            if (!n) { add_log("scope config: bad value in '" + args[i] + "'"); return; }
            if (key == "rate") { c.sample_rate_hz = *n; have_rate = true; }
            else if (key == "pre") { c.pre_trigger_samples = *n; }
            else if (key == "post") { c.post_trigger_samples = *n; have_post = true; }
            else { add_log("scope config: unknown setting '" + key + "'"); return; }
        }
        if (c.channels_count == 0 || !have_rate || !have_post) {
            add_log("Usage: scope config ch=<adc>:<input>[:<smp>]... rate=<Hz> pre=<n> post=<n> "
                    "[trig=sw,awd,comp<N>,ext] [awd=<ch>:<lo>:<hi>] [fullpre=0|1]");
            return;
        }
    } else if (sub == "read") {
        const auto state = connector.get_state();
        const bool done = state.has_scope_status && state.scope_status.state == ScopeState_SCOPE_STATE_DONE;
        uint32_t mask = done ? (1u << state.scope_status.num_channels) - 1u : 0;
        int64_t start = done ? state.scope_status.first_sample : 0;
        uint32_t count = done ? static_cast<uint32_t>(state.scope_status.post_trigger_samples - state.scope_status.first_sample) : 0;
        const uint32_t rate = done ? state.scope_status.sample_rate_hz : 0;
        DataLink link = connector.aux_open() ? DataLink_DATA_LINK_AUX : DataLink_DATA_LINK_ORIGIN;
        std::string file = std::format("scope_{}.csv", std::time(nullptr));
        for (size_t i = 1; i < args.size(); ++i) {
            const auto [key, value] = key_value(args[i]);
            if (key == "file") { file = value; continue; }
            if (key == "link") {
                if (value == "aux") link = DataLink_DATA_LINK_AUX;
                else if (value == "origin" || value == "ctrl") link = DataLink_DATA_LINK_ORIGIN;
                else { add_log("scope read: link=origin|aux"); return; }
                continue;
            }
            if (key == "start") {
                const auto v = parse_number<int64_t>(value);
                if (!v) { add_log("scope read: bad start"); return; }
                start = *v;
                continue;
            }
            const auto v = parse_number<uint32_t>(value);
            if (!v) { add_log("scope read: bad value in '" + args[i] + "'"); return; }
            if (key == "ch") mask = *v;
            else if (key == "count") count = *v;
            else { add_log("scope read: unknown setting '" + key + "'"); return; }
        }
        if (mask == 0 || count == 0) {
            add_log("scope read: no finished capture known - run 'scope status' or give ch=<mask> start=<n> count=<n>");
            return;
        }
        if (link == DataLink_DATA_LINK_AUX && !connector.aux_open()) {
            add_log("scope read: link=aux needs --aux-port");
            return;
        }
        std::string error;
        if (!scope_client.read(mask, start, count, rate, link, file, error)) {
            add_log("scope read: " + error);
            return;
        }
        add_log(std::format("Scope: reading {} samples x mask 0x{:x} from {} into {}", count, mask, start, file));
        return;
    } else {
        add_log("Usage: scope config|arm|trigger|stop|status|read|cancel (help scope)");
        return;
    }
    connector.send(req);
}

void Tui::handle_link(const std::vector<std::string>& args) noexcept {
    if (args.size() == 2 && args[0] == "baud") {
        const auto baud = parse_number<uint32_t>(args[1]);
        if (!baud || *baud == 0) {
            add_log("link baud: bad rate");
            return;
        }
        Request req = Request_init_zero;
        req.which_command = Request_data_link_configure_tag;
        req.command.data_link_configure.baudrate = *baud;
        requested_aux_baud = *baud;   // host port follows when the device confirms
        connector.send(req);
        return;
    }
    if (args.size() == 1 && args[0] == "status") {
        const auto st = connector.get_state();
        add_log(std::format("Control link: {} frames, {} CRC errors, {} header errors, {} bytes dropped",
                            st.link_stats.frames, st.link_stats.crc_errors, st.link_stats.header_errors,
                            st.link_stats.dropped_bytes));
        add_log(std::format("Aux link: {} ({}), {} frames, {} CRC errors, {} bytes dropped",
                            connector.aux_open() ? "open" : "closed",
                            st.aux_baudrate ? std::format("device {} Bd", st.aux_baudrate) : std::string("device baud unknown"),
                            st.aux_stats.frames, st.aux_stats.crc_errors, st.aux_stats.dropped_bytes));
        return;
    }
    add_log("Usage: link baud <rate> | link status");
}

void Tui::handle_applet_start(const std::string& args) noexcept {
    BOOST_LOG_TRIVIAL(debug) << "Tui::handle_applet_start() with args: \"" << args << "\"";

    // Find first space to separate applet name from arguments
    size_t space_pos = args.find(' ');
    std::string applet_name = args.substr(0, space_pos);
    std::string applet_args = (space_pos != std::string::npos) ? args.substr(space_pos + 1) : "";

    if (applet_name.empty()) {
        add_log("Error: applet name required");
        return;
    }

    // Build Request message
    Request req = Request_init_default;
    req.which_command = Request_applet_start_tag;

    // Set applet name - copy to fixed-size char array
    std::strncpy(req.command.applet_start.applet_name, applet_name.c_str(), sizeof(req.command.applet_start.applet_name) - 1);
    req.command.applet_start.applet_name[sizeof(req.command.applet_start.applet_name) - 1] = '\0';

    // Set arguments if any
    if (!applet_args.empty()) {
        std::vector<std::string> tokens;
        size_t pos = 0;
        while (pos < applet_args.size()) {
            // Skip leading whitespace
            while (pos < applet_args.size() && applet_args[pos] == ' ') pos++;
            if (pos >= applet_args.size()) break;

            std::string token;
            bool in_quote = false;

            while (pos < applet_args.size()) {
                char c = applet_args[pos];
                if (c == '"') {
                    in_quote = !in_quote;
                    pos++;
                } else if (c == ' ' && !in_quote) {
                    pos++;
                    break;
                } else {
                    token += c;
                    pos++;
                }
            }

            if (!token.empty() || in_quote) {
                tokens.push_back(token);
            }
        }

        req.command.applet_start.args_count = std::min(tokens.size(), (size_t)16);
        for (size_t i = 0; i < req.command.applet_start.args_count; ++i) {
            std::strncpy(req.command.applet_start.args[i], tokens[i].c_str(), sizeof(req.command.applet_start.args[0]) - 1);
            req.command.applet_start.args[i][sizeof(req.command.applet_start.args[0]) - 1] = '\0';
        }
    }

    connector.send(req);
    add_log(std::string("Starting applet: ") + applet_name);
    if (!applet_args.empty()) {
        add_log(std::string("Arguments: ") + applet_args);
    }
}

void Tui::handle_applet_stop(const std::string& args) noexcept {
    BOOST_LOG_TRIVIAL(debug) << "Tui::handle_applet_stop() with args: \"" << args << "\"";
    
    std::string applet_name = args;
    // Trim whitespace
    while (!applet_name.empty() && isspace(applet_name.back())) {
        applet_name.pop_back();
    }
    
    if (applet_name.empty()) {
        add_log("Error: applet name required");
        return;
    }

    // Build Request message
    Request req = Request_init_default;
    req.which_command = Request_applet_stop_tag;

    // Copy to fixed-size char array
    std::strncpy(req.command.applet_stop.applet_name, applet_name.c_str(), sizeof(req.command.applet_stop.applet_name) - 1);
    req.command.applet_stop.applet_name[sizeof(req.command.applet_stop.applet_name) - 1] = '\0';

    connector.send(req);
    add_log(std::string("Stopping applet: ") + applet_name);
}


void Tui::handle_applet_command(const std::string& args) noexcept {
    BOOST_LOG_TRIVIAL(debug) << "Tui::handle_applet_command() with args: \"" << args << "\"";

    // Find first space to separate applet name from arguments
    size_t space_pos = args.find(' ');
    std::string applet_name = args.substr(0, space_pos);
    std::string applet_args = (space_pos != std::string::npos) ? args.substr(space_pos + 1) : "";

    if (applet_name.empty()) {
        add_log("Error: applet name required");
        return;
    }

    // Build Request message
    Request req = Request_init_default;
    req.which_command = Request_applet_command_tag;

    // Copy to fixed-size char array
    std::strncpy(req.command.applet_command.applet_name, applet_name.c_str(), sizeof(req.command.applet_command.applet_name) - 1);
    req.command.applet_command.applet_name[sizeof(req.command.applet_command.applet_name) - 1] = '\0';

    // Set arguments if any
    if (!applet_args.empty()) {
        std::vector<std::string> tokens;
        size_t pos = 0;
        while (pos < applet_args.size()) {
            // Skip leading whitespace
            while (pos < applet_args.size() && applet_args[pos] == ' ') pos++;
            if (pos >= applet_args.size()) break;

            std::string token;
            bool in_quote = false;

            while (pos < applet_args.size()) {
                char c = applet_args[pos];
                if (c == '"') {
                    in_quote = !in_quote;
                    pos++;
                } else if (c == ' ' && !in_quote) {
                    pos++;
                    break;
                } else {
                    token += c;
                    pos++;
                }
            }

            if (!token.empty() || in_quote) {
                tokens.push_back(token);
            }
        }

        req.command.applet_command.args_count = std::min(tokens.size(), (size_t)16);
        for (size_t i = 0; i < req.command.applet_command.args_count; ++i) {
            std::strncpy(req.command.applet_command.args[i], tokens[i].c_str(), sizeof(req.command.applet_command.args[0]) - 1);
            req.command.applet_command.args[i][sizeof(req.command.applet_command.args[0]) - 1] = '\0';
        }
    }

    connector.send(req);
    add_log(std::string("Sending command to applet: ") + applet_name);
    if (!applet_args.empty()) {
        add_log(std::string("Arguments: ") + applet_args);
    }
}

void Tui::handle_applet_list(const std::string& args) noexcept {
    (void)args;  // Unused
    BOOST_LOG_TRIVIAL(debug) << "Tui::handle_applet_list() called";

    // Build Request message
    Request req = Request_init_default;
    req.which_command = Request_applet_list_tag;

    connector.send(req);
    add_log("Requesting applet list...");
}

void Tui::show_help(const std::string& command) noexcept {
    if (command == "scope") {
        add_log("Oscilloscope (captures go to PSRAM, read them back with 'scope read'):");
        add_log("  scope config ch=<adc>:<input>[:<smp>] [ch=...] rate=<Hz> pre=<n> post=<n>");
        add_log("               [trig=sw,awd,comp<N>,ext] [awd=<ch>:<low>:<high>] [fullpre=0|1]");
        add_log("      smp: 0..7 = 2.5/6.5/12.5/24.5/47.5/92.5/247.5/640.5 ADC cycles; up to 4 channels");
        add_log("  scope arm | trigger | stop | status");
        add_log("  scope read [file=<csv>] [ch=<mask>] [start=<n>] [count=<n>] [link=origin|aux]");
        add_log("      defaults: whole capture of the last 'scope status', aux link if --aux-port is open");
        add_log("      lost frames are re-requested by range automatically");
        add_log("  scope cancel");
        return;
    }
    if (!command.empty() && command != "all") {
        add_log("No help for '" + command + "'");
        return;
    }
    add_log("Commands:");
    add_log("  help [scope]                 This help");
    add_log("  ping | status                Ping / request system status");
    add_log("  poll <seconds>|off           Periodic status requests");
    add_log("  charger on|off               Charging enable");
    add_log("  charger set [vchg=V] [ichg=A] [vin=V] [iin=A] [src=usb|ac1|ac2|auto]");
    add_log("  out on|off                   Output regulator enable");
    add_log("  out set [v=V] [ilim=A] [rsense=ohm]");
    add_log("  scope ...                    Oscilloscope, see 'help scope'");
    add_log("  link baud <rate> | status    Aux data link baud rate / link statistics");
    add_log("  list | start <applet> [args] | stop <applet> | <applet> [args]");
}

void Tui::print_command_tree(const std::vector<CommandNode>& nodes, int indent) noexcept {
    (void)nodes;
    (void)indent;
    // Not implemented yet
}

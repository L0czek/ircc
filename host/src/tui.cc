#include "tui.hpp"
#include "cli.hpp"
#include "connector.hpp"
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
    screen(ftxui::ScreenInteractive::Fullscreen())
{
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

    connector.async_run_receiver();

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

ftxui::Component Tui::render_status() const noexcept {
    return ftxui::Renderer([this] {
        std::string status_text;
        std::string heartbeat;

        auto now = std::time(nullptr);
        if (now - last_heartbeat.load() == 0) {
            heartbeat = "\u25CF";
        } else {
            heartbeat = "\u25CB";
        }

        const auto state = connector.get_state();

        if (command_mode) {
            status_text = "Command mode | Type command and press Enter | Esc to cancel";
        } else {
            status_text = "Status: Normal | Type to enter command | Last ping: ";
            if (state.last_ping == 0) {
                status_text += "Not connected";
            } else {
                status_text += std::format("{}s ago", now - state.last_ping);
            }
        }

        status_text += " | Heartbeat: " + heartbeat;

        // Build main container with all components arranged horizontally
        std::vector<ftxui::Element> status_components;

        // Charger section
        std::vector<ftxui::Element> charger_rows;
        charger_rows.push_back(ftxui::text(" Charger "));
        charger_rows.push_back(ftxui::text("-----------"));

        std::string charger_enabled = state.system_status.has_charger ?
            (state.system_status.charger.enabled ? "[ON]" : "[OFF]") : "[?]";
        charger_rows.push_back(ftxui::text("  " + charger_enabled));

        charger_rows.push_back(ftxui::text(
            std::format("  VBAT: {:.2f}V", state.system_status.charger_measurements.vbat_voltage)
        ));

        charger_rows.push_back(ftxui::text(
            std::format("  IBUS: {:.2f}A", state.system_status.charger_measurements.ibus_current)
        ));

        charger_rows.push_back(ftxui::text(
            std::format("  VBUS: {:.2f}V", state.system_status.charger_measurements.vbus_voltage)
        ));

        {
            std::string charge_status;
            switch (state.system_status.charger.charge_status) {
                case ChargeStatus_CHARGE_STATUS_NOT_CHARGING: charge_status = "Not Charging"; break;
                case ChargeStatus_CHARGE_STATUS_TRICKLE_CHARGE: charge_status = "Trickle Charge"; break;
                case ChargeStatus_CHARGE_STATUS_PRECHARGE: charge_status = "Precharge"; break;
                case ChargeStatus_CHARGE_STATUS_FAST_CHARGE: charge_status = "Fast Charge"; break;
                case ChargeStatus_CHARGE_STATUS_TAPER_CHARGE: charge_status = "Taper Charge"; break;
                case ChargeStatus_CHARGE_STATUS_TOP_OFF: charge_status = "Top Off"; break;
                case ChargeStatus_CHARGE_STATUS_CHARGE_DONE: charge_status = "Charged"; break;
                default: charge_status = "Unknown"; break;
            }
            charger_rows.push_back(ftxui::text("  Status: " + charge_status));
        }

        status_components.push_back(ftxui::vbox(charger_rows) | ftxui::border);

        // Output Regulator section (renamed from BackBoost)
        std::vector<ftxui::Element> output_regulator_rows;
        output_regulator_rows.push_back(ftxui::text(" Output Regulator "));
        output_regulator_rows.push_back(ftxui::text("------------------"));

        std::string output_regulator_enabled = state.system_status.has_output_regulator ?
            (state.system_status.output_regulator.enabled ? "[ON]" : "[OFF]") : "[?]";
        output_regulator_rows.push_back(ftxui::text("  " + output_regulator_enabled));

        output_regulator_rows.push_back(ftxui::text(
            std::format("  VOUT: {:.2f}V", state.system_status.output_regulator_measurements.output_voltage)
        ));

        output_regulator_rows.push_back(ftxui::text(
            std::format("  IOUT: {:.2f}A", state.system_status.output_regulator_measurements.output_current)
        ));

        {
            std::string mode;
            switch (state.system_status.output_regulator.operating_mode) {
                case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK: mode = "Buck"; break;
                case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BOOST: mode = "Boost"; break;
                case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK_BOOST: mode = "Buck-Boost"; break;
                default: mode = "Unknown"; break;
            }
            output_regulator_rows.push_back(ftxui::text("  Mode: " + mode));
        }

        status_components.push_back(ftxui::vbox(output_regulator_rows) | ftxui::border);

        // DAC section
        std::vector<ftxui::Element> dac_rows;
        dac_rows.push_back(ftxui::text(" DAC "));
        dac_rows.push_back(ftxui::text("-----"));

        std::string dac_enabled = state.system_status.has_dac ?
            (state.system_status.dac.enabled ? "[ON]" : "[OFF]") : "[?]";
        dac_rows.push_back(ftxui::text("  " + dac_enabled));

        {
            std::string mode;
            switch (state.system_status.dac.mode) {
                case DACMode_DAC_MODE_VOLTAGE: mode = "Voltage"; break;
                case DACMode_DAC_MODE_WAVEFORM: mode = "Waveform"; break;
                default: mode = "Unknown"; break;
            }
            dac_rows.push_back(ftxui::text("  Mode: " + mode));
        }

        dac_rows.push_back(ftxui::text(
            std::format("  Output: {:.3f}V", state.system_status.dac_measurements.voltage_output)
        ));

        status_components.push_back(ftxui::vbox(dac_rows) | ftxui::border);

        return ftxui::hbox(status_components) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80)
            | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 10);
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
    }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80);
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
    }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 3);
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

        return graph_box | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80);
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

void Tui::schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period, PeriodicTask::callback_t func) noexcept {
    tasks.emplace(type, PeriodicTask(io_ctx, period, func, connector));
}

void Tui::execute_command(const std::string& command_line) noexcept {
    BOOST_LOG_TRIVIAL(debug) << "Tui::execute_command() called with: \"" << command_line << "\"";
    add_log("Command: " + command_line);

    // Simple help command
    if (command_line == "help" || command_line == "h") {
        BOOST_LOG_TRIVIAL(debug) << "Executing help command";
        show_help();
        return;
    }

    // Applet list: "list"
    if (command_line == "list") {
        handle_applet_list("");
        return;
    }

    // Applet start: "start <name> [args...]"
    if (command_line.starts_with("start ")) {
        handle_applet_start(command_line.substr(6));
        return;
    }

    // Applet stop: "stop <name>"
    if (command_line.starts_with("stop ")) {
        handle_applet_stop(command_line.substr(5));
        return;
    }

    // Direct applet command: "<name> [args...]" - first token is applet name
    // The handler will parse the tokens
    if (!command_line.empty() && !command_line.starts_with("status") && !command_line.starts_with("ping")) {
        handle_applet_command(command_line);
        return;
    }

    add_log("Unknown command: " + command_line);
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
    BOOST_LOG_TRIVIAL(debug) << "Tui::show_help() called with command: \"" << command << "\"";
    if (command.empty()) {
        BOOST_LOG_TRIVIAL(debug) << "Showing general help";
        add_log("Available commands:");
        add_log("  help                    - Show this help message");
        add_log("  status                  - Request device status");
        add_log("  ping                    - Send ping command");
        add_log("  list                    - List all available applets");
        add_log("  start <name> [args...]  - Start an applet");
        add_log("  stop <name>             - Stop an applet");
        add_log("  <name> [args...]        - Send command to applet");
    } else {
        add_log("Unknown command: " + command);
    }
}

void Tui::print_command_tree(const std::vector<CommandNode>& nodes, int indent) noexcept {
    (void)nodes;
    (void)indent;
    // Not implemented yet
}

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
#include <stdexcept>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

Tui::Tui(const Options &cfg) :
    io_ctx(),
    connector(io_ctx, std::bind(&Tui::refresh, this), std::bind(&Tui::add_log, this, std::placeholders::_1)),
    screen(ftxui::ScreenInteractive::FitComponent())
{
    BOOST_LOG_TRIVIAL(info) << "Connecting...\n";
    if (!connector.open(cfg)) {
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
}

void Tui::focus_next() noexcept {
    if (modal_state == ModalState::ChargerMenu) {
        // In charger menu, focus is handled by charger_menu_selection
        return;
    }
    
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
    if (modal_state == ModalState::ChargerMenu) {
        // In charger menu, focus is handled by charger_menu_selection
        return;
    }
    
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
    screen.PostEvent(ftxui::Event::Custom);
}

void Tui::run() noexcept {
    auto root = renderer();

    // Wrap the renderer with event handling
    auto event_handler = ftxui::CatchEvent(root, [this](ftxui::Event event) {
        // Handle character input for command input mode
        if (event.is_character()) {
            if (modal_state == ModalState::CommandInput) {
                command_input += event.character();
                return true;
            }
        }

        // Handle charger menu navigation (has priority over other modes)
        if (modal_state == ModalState::ChargerMenu) {
            if (event == ftxui::Event::ArrowDown) {
                charger_menu_selection = (charger_menu_selection + 1) % 5;
                return true;
            }

            if (event == ftxui::Event::ArrowUp) {
                charger_menu_selection = (charger_menu_selection + 4) % 5;
                return true;
            }

            if (event == ftxui::Event::Return) {
                switch (charger_menu_selection) {
                    case 0: // Refresh Status
                        request_charger_status();
                        break;
                    case 1: // Refresh Measurements
                        request_charger_measurements();
                        break;
                    case 2: // Refresh Config
                        request_charger_config();
                        break;
                    case 3: // Edit Config
                        exit_charger_menu();
                        command_input = "config:";
                        set_modal_state(ModalState::CommandInput);
                        return true;
                    case 4: // Exit
                        exit_charger_menu();
                        break;
                }
                return true;
            }

            if (event == ftxui::Event::Escape) {
                exit_charger_menu();
                return true;
            }
        }

        // Handle BackBoost menu navigation
        if (modal_state == ModalState::BackBoostMenu) {
            if (event == ftxui::Event::ArrowDown) {
                backboost_menu_selection = (backboost_menu_selection + 1) % 5;
                return true;
            }

            if (event == ftxui::Event::ArrowUp) {
                backboost_menu_selection = (backboost_menu_selection + 4) % 5;
                return true;
            }

            if (event == ftxui::Event::Return) {
                switch (backboost_menu_selection) {
                    case 0: // Refresh Status
                        request_backboost_status();
                        break;
                    case 1: // Refresh Measurements
                        request_backboost_measurements();
                        break;
                    case 2: // Refresh Config
                        request_backboost_config();
                        break;
                    case 3: // Edit Config
                        exit_backboost_menu();
                        command_input = "bbconfig:";
                        set_modal_state(ModalState::CommandInput);
                        return true;
                    case 4: // Exit
                        exit_backboost_menu();
                        break;
                }
                return true;
            }

            if (event == ftxui::Event::Escape) {
                exit_backboost_menu();
                return true;
            }
        }

        // Handle DAC menu navigation
        if (modal_state == ModalState::DacMenu) {
            if (event == ftxui::Event::ArrowDown) {
                dac_menu_selection = (dac_menu_selection + 1) % 5;
                return true;
            }

            if (event == ftxui::Event::ArrowUp) {
                dac_menu_selection = (dac_menu_selection + 4) % 5;
                return true;
            }

            if (event == ftxui::Event::Return) {
                switch (dac_menu_selection) {
                    case 0: // Refresh Status
                        request_dac_status();
                        break;
                    case 1: // Refresh Measurements
                        request_dac_measurements();
                        break;
                    case 2: // Refresh Config
                        request_dac_config();
                        break;
                    case 3: // Edit Config
                        exit_dac_menu();
                        command_input = "dacconfig:";
                        set_modal_state(ModalState::CommandInput);
                        return true;
                    case 4: // Exit
                        exit_dac_menu();
                        break;
                }
                return true;
            }

            if (event == ftxui::Event::Escape) {
                exit_dac_menu();
                return true;
            }
        }

        // Handle global navigation keys
        if (event == ftxui::Event::ArrowDown) {
            focus_next();
            return true;
        }

        if (event == ftxui::Event::ArrowUp) {
            focus_prev();
            return true;
        }

        // Handle command input mode
        if (modal_state == ModalState::CommandInput) {
            if (event == ftxui::Event::Backspace) {
                if (!command_input.empty()) {
                    command_input.pop_back();
                }
                return true;
            }

            if (event == ftxui::Event::Return) {
                if (!command_input.empty()) {
                    add_log("Command: " + command_input);

                    if (command_input == "ping") {
                        Request req = Request_init_default;
                        req.which_command = Request_ping_tag;
                        connector.send(req);
                        add_log("Sent: ping");
                    } else if (command_input == "config:") {
                        // Enter charger menu
                        enter_charger_menu();
                    } else if (command_input == "bbconfig:") {
                        // Enter BackBoost menu
                        enter_backboost_menu();
                    } else if (command_input == "dacconfig:") {
                        // Enter DAC menu
                        enter_dac_menu();
                    } else if (command_input.substr(0, 7) == "config:" && command_input.length() > 7) {
                        // Parse charger config commands like "config:v=12.5,i=2.0"
                        Request req = Request_init_default;
                        req.which_command = Request_set_charger_config_tag;

                        std::string config_str = command_input.substr(7);
                        // Simple parsing: split by commas and parse key=value pairs
                        // Format: v=12.5,i=2.0,iv=13.5,ii=2.5
                        // This is a simplified parser - could be enhanced

                        add_log("Config command parsed (simplified)");
                        // Note: Full parsing would require more sophisticated logic
                        // For now, just request current config
                        Request get_req = Request_init_default;
                        get_req.which_command = Request_get_charger_config_tag;
                        connector.send(get_req);
                    } else {
                        add_log("Unknown command: " + command_input);
                    }

                    command_input.clear();
                    set_modal_state(ModalState::Normal);
                }
                return true;
            }

            if (event == ftxui::Event::Escape) {
                command_input.clear();
                set_modal_state(ModalState::Normal);
                return true;
            }
        } else {
            // Enter command input mode with ':'
            if (event.character() == ":") {
                set_modal_state(ModalState::CommandInput);
                return true;
            }
        }

        return false;
    });

    ftxui::Loop loop(&screen, event_handler);
    auto handle = std::thread([this] {
        io_ctx.run();
        BOOST_LOG_TRIVIAL(info) << "io service exited\n";
    });

    while (true) {
        loop.RunOnceBlocking();
        std::this_thread::sleep_for(100ms);

        auto now = std::time(nullptr);
        if (now - last_heartbeat >= 1) {
            last_heartbeat = now;
            refresh();
        }
    }

    handle.join();
}

void Tui::add_log(const std::string &line) noexcept {
    std::lock_guard<std::mutex> lock(log_mutex);
    log_buffer.push_back(line);
    
    if (log_buffer.size() > 100) {
        log_buffer.pop_front();
    }
    
    refresh();
}

void Tui::set_modal_state(ModalState state) noexcept {
    modal_state = state;
    refresh();
}

ftxui::Component Tui::render_status() const noexcept {
    return ftxui::Renderer([this] {
        std::string status_text;
        std::string heartbeat;

        auto now = std::time(nullptr);
        if (now - last_heartbeat == 0) {
            heartbeat = "\u25CF";
        } else {
            heartbeat = "\u25CB";
        }

        switch (modal_state) {
            case ModalState::Normal:
                status_text = "Status: Normal | Press ':' for commands | Last ping: ";
                if (connector.get_state().last_ping == 0) {
                    status_text += "Not connected";
                } else {
                    status_text += std::format("{}s ago", now - connector.get_state().last_ping);
                }
                break;
            case ModalState::CommandInput:
                status_text = "Status: Command Input | Type command and press Enter | Esc to cancel";
                break;
            case ModalState::ChargerMenu:
                status_text = "Status: Charger Menu | Use Arrow keys to navigate | Enter to select | Esc to exit";
                break;
            case ModalState::BackBoostMenu:
                status_text = "Status: BackBoost Menu | Use Arrow keys to navigate | Enter to select | Esc to exit";
                break;
            case ModalState::DacMenu:
                status_text = "Status: DAC Menu | Use Arrow keys to navigate | Enter to select | Esc to exit";
                break;
        }

        status_text += " | Heartbeat: " + heartbeat;

        auto element = ftxui::text(status_text) | ftxui::border;

        if (focused == FocusedComponent::Status) {
            element = element | ftxui::borderHeavy;
        }

        return element | ftxui::color(ftxui::Color::Yellow);
    });
}

ftxui::Component Tui::render_log() const noexcept {
    return ftxui::Renderer([this] {
        std::lock_guard<std::mutex> lock(log_mutex);

        std::vector<ftxui::Element> log_elements;
        size_t count = 0;

        for (auto it = log_buffer.rbegin(); it != log_buffer.rend() && count < 20; ++it, ++count) {
            log_elements.push_back(ftxui::text(*it));
        }

        if (log_buffer.empty()) {
            log_elements.push_back(ftxui::text("No logs yet..."));
        }

        auto element = ftxui::vbox(log_elements) | ftxui::border;

        if (focused == FocusedComponent::Log) {
            element = element | ftxui::borderHeavy;
        }

        return element | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 15);
    });
}

ftxui::Component Tui::render_command_input() const noexcept {
    return ftxui::Renderer([this] {
        if (modal_state != ModalState::CommandInput) {
            auto element = ftxui::text("Press ':' to enter command mode");

            if (focused == FocusedComponent::Command) {
                element = element | ftxui::borderHeavy;
            }

            return element | ftxui::border;
        }

        std::string prompt = ": ";
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
    });
}

ftxui::Component Tui::render_charger_status() const noexcept {
    return ftxui::Renderer([this] {
        const auto& state = connector.get_state();

        std::vector<ftxui::Element> rows;
        
        // Battery voltage
        if (state.charger_measurements.has_vbat_voltage) {
            rows.push_back(ftxui::text(std::format("Battery Voltage: {:.2f} V", state.charger_measurements.vbat_voltage)));
        }
        
        // VBUS voltage
        if (state.charger_measurements.has_vbus_voltage) {
            rows.push_back(ftxui::text(std::format("VBUS Voltage: {:.2f} V", state.charger_measurements.vbus_voltage)));
        }
        
        // System voltage
        if (state.charger_measurements.has_vsys_voltage) {
            rows.push_back(ftxui::text(std::format("System Voltage: {:.2f} V", state.charger_measurements.vsys_voltage)));
        }
        
        // Charge current
        if (state.charger_measurements.has_ibus_current) {
            rows.push_back(ftxui::text(std::format("Charge Current: {:.2f} A", state.charger_measurements.ibus_current)));
        }
        
        // Status info
        if (state.charger_status.has_enabled) {
            rows.push_back(ftxui::text(std::format("Charger Enabled: {}", state.charger_status.enabled ? "Yes" : "No")));
        }
        
        if (state.charger_status.has_plugged_in) {
            rows.push_back(ftxui::text(std::format("Input Plugged: {}", state.charger_status.plugged_in ? "Yes" : "No")));
        }
        
        if (state.charger_status.has_battery_present) {
            rows.push_back(ftxui::text(std::format("Battery Present: {}", state.charger_status.battery_present ? "Yes" : "No")));
        }
        
        if (state.charger_status.has_fault_present) {
            rows.push_back(ftxui::text(std::format("Fault Present: {}", state.charger_status.fault_present ? "Yes" : "No")));
        }
        
        // Charge status
        if (state.charger_status.has_charge_status) {
            const char* charge_status_str = "Unknown";
            switch (state.charger_status.charge_status) {
                case ChargeStatus_CHARGE_STATUS_NOT_CHARGING: charge_status_str = "Not Charging"; break;
                case ChargeStatus_CHARGE_STATUS_TRICKLE_CHARGE: charge_status_str = "Trickle Charge"; break;
                case ChargeStatus_CHARGE_STATUS_PRECHARGE: charge_status_str = "Precharge"; break;
                case ChargeStatus_CHARGE_STATUS_FAST_CHARGE: charge_status_str = "Fast Charge"; break;
                case ChargeStatus_CHARGE_STATUS_TAPER_CHARGE: charge_status_str = "Taper Charge"; break;
                case ChargeStatus_CHARGE_STATUS_TOP_OFF: charge_status_str = "Top Off"; break;
                case ChargeStatus_CHARGE_STATUS_CHARGE_DONE: charge_status_str = "Charge Done"; break;
            }
            rows.push_back(ftxui::text(std::format("Charge Status: {}", charge_status_str)));
        }
        
        // VBUS status
        if (state.charger_status.has_vbus_status) {
            const char* vbus_status_str = "Unknown";
            switch (state.charger_status.vbus_status) {
                case VBUSStatus_VBUS_STATUS_NO_INPUT: vbus_status_str = "No Input"; break;
                case VBUSStatus_VBUS_STATUS_USB_SDP_500MA: vbus_status_str = "USB SDP (500mA)"; break;
                case VBUSStatus_VBUS_STATUS_USB_CDP_1500MA: vbus_status_str = "USB CDP (1.5A)"; break;
                case VBUSStatus_VBUS_STATUS_USB_DCP_3250MA: vbus_status_str = "USB DCP (3.25A)"; break;
                case VBUSStatus_VBUS_STATUS_ADJUSTABLE_HIGH: vbus_status_str = "Adjustable High"; break;
                case VBUSStatus_VBUS_STATUS_UNKNOWN_ADAPTER: vbus_status_str = "Unknown Adapter"; break;
                case VBUSStatus_VBUS_STATUS_NON_STANDARD_ADAPTER: vbus_status_str = "Non-Standard Adapter"; break;
                case VBUSStatus_VBUS_STATUS_OTG_MODE: vbus_status_str = "OTG Mode"; break;
                case VBUSStatus_VBUS_STATUS_NOT_QUALIFIED_ADAPTER: vbus_status_str = "Not Qualified"; break;
                case VBUSStatus_VBUS_STATUS_POWERED_FROM_VBUS: vbus_status_str = "Powered from VBUS"; break;
            }
            rows.push_back(ftxui::text(std::format("VBUS Status: {}", vbus_status_str)));
        }
        
        // Input source
        if (state.charger_status.has_input_source) {
            const char* input_source_str = "Unknown";
            switch (state.charger_status.input_source) {
                case ChargerInputSource_CHARGER_INPUT_USB_DPDM: input_source_str = "USB DPDM"; break;
                case ChargerInputSource_CHARGER_INPUT_AC1: input_source_str = "AC1"; break;
                case ChargerInputSource_CHARGER_INPUT_AC2: input_source_str = "AC2"; break;
                case ChargerInputSource_CHARGER_INPUT_AUTO: input_source_str = "Auto"; break;
            }
            rows.push_back(ftxui::text(std::format("Input Source: {}", input_source_str)));
        }
        
        auto element = ftxui::vbox(rows) | ftxui::border;
        return element | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 20);
    });
}

ftxui::Component Tui::render_charger_menu() const noexcept {
    return ftxui::Renderer([this] {
        std::vector<ftxui::Element> menu_items;
        
        // Menu options
        const char* options[] = {
            "Refresh Status",
            "Refresh Measurements",
            "Refresh Config",
            "Edit Config",
            "Exit"
        };
        
        for (size_t i = 0; i < 5; ++i) {
            std::string prefix = (charger_menu_selection == static_cast<int>(i)) ? " > " : "   ";
            menu_items.push_back(ftxui::text(prefix + options[i]));
        }
        
        auto menu_element = ftxui::vbox(menu_items) | ftxui::border;
        
        if (charger_menu_active) {
            menu_element = menu_element | ftxui::borderHeavy;
        }
        
        return menu_element | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 10);
    });
}

ftxui::Component Tui::render_charger_config_input() const noexcept {
    return ftxui::Renderer([this] {
        std::vector<ftxui::Element> rows;
        
        // Config input fields
        rows.push_back(ftxui::text(std::format("Charge Voltage (V): [{}] (3.5-19.2V)", charge_voltage_input)));
        rows.push_back(ftxui::text(std::format("Charge Current (A): [{}] (0-12.79A)", charge_current_input)));
        rows.push_back(ftxui::text(std::format("Input Voltage (V): [{}] (3.9-25.5V)", input_voltage_input)));
        rows.push_back(ftxui::text(std::format("Input Current (A): [{}] (0-12.79A)", input_current_input)));
        
        rows.push_back(ftxui::text(""));
        rows.push_back(ftxui::text("Press Enter to send config"));
        rows.push_back(ftxui::text("Press Esc to cancel"));
        
        auto element = ftxui::vbox(rows) | ftxui::border;
        return element | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 12);
    });
}

ftxui::Component Tui::render_backboost_status() const noexcept {
    return ftxui::Renderer([this] {
        const auto& state = connector.get_state();

        std::vector<ftxui::Element> rows;

        // Output voltage
        if (state.backboost_measurements.has_output_voltage) {
            rows.push_back(ftxui::text(std::format("Output Voltage: {:.2f} V", state.backboost_measurements.output_voltage)));
        }

        // Output current
        if (state.backboost_measurements.has_output_current) {
            rows.push_back(ftxui::text(std::format("Output Current: {:.2f} A", state.backboost_measurements.output_current)));
        }

        // Status info
        if (state.backboost_status.has_enabled) {
            rows.push_back(ftxui::text(std::format("Output Enabled: {}", state.backboost_status.enabled ? "Yes" : "No")));
        }

        if (state.backboost_status.has_fault_present) {
            rows.push_back(ftxui::text(std::format("Fault Present: {}", state.backboost_status.fault_present ? "Yes" : "No")));
        }

        if (state.backboost_status.has_scp_fault) {
            rows.push_back(ftxui::text(std::format("SCP Fault: {}", state.backboost_status.scp_fault ? "Yes" : "No")));
        }

        if (state.backboost_status.has_ocp_fault) {
            rows.push_back(ftxui::text(std::format("OCP Fault: {}", state.backboost_status.ocp_fault ? "Yes" : "No")));
        }

        if (state.backboost_status.has_ovp_fault) {
            rows.push_back(ftxui::text(std::format("OVP Fault: {}", state.backboost_status.ovp_fault ? "Yes" : "No")));
        }

        // Operating mode
        if (state.backboost_status.has_operating_mode) {
            const char* operating_mode_str = "Unknown";
            switch (state.backboost_status.operating_mode) {
                case BackBoostOperatingMode_BACK_BOOST_OPERATING_MODE_BUCK: operating_mode_str = "Buck"; break;
                case BackBoostOperatingMode_BACK_BOOST_OPERATING_MODE_BOOST: operating_mode_str = "Boost"; break;
                case BackBoostOperatingMode_BACK_BOOST_OPERATING_MODE_BUCK_BOOST: operating_mode_str = "Buck-Boost"; break;
                case BackBoostOperatingMode_BACK_BOOST_OPERATING_MODE_FREQUENCY_LIMIT: operating_mode_str = "Frequency Limit"; break;
            }
            rows.push_back(ftxui::text(std::format("Operating Mode: {}", operating_mode_str)));
        }

        // Feedback mode
        if (state.backboost_status.has_feedback_mode) {
            const char* feedback_mode_str = "Unknown";
            switch (state.backboost_status.feedback_mode) {
                case BackBoostFeedbackMode_BACK_BOOST_FEEDBACK_MODE_PWM: feedback_mode_str = "PWM"; break;
                case BackBoostFeedbackMode_BACK_BOOST_FEEDBACK_MODE_PFM: feedback_mode_str = "PFM"; break;
            }
            rows.push_back(ftxui::text(std::format("Feedback Mode: {}", feedback_mode_str)));
        }

        auto element = ftxui::vbox(rows) | ftxui::border;
        return element | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 20);
    });
}

ftxui::Component Tui::render_backboost_menu() const noexcept {
    return ftxui::Renderer([this] {
        std::vector<ftxui::Element> menu_items;

        // Menu options
        const char* options[] = {
            "Refresh Status",
            "Refresh Measurements",
            "Refresh Config",
            "Edit Config",
            "Exit"
        };

        for (size_t i = 0; i < 5; ++i) {
            std::string prefix = (backboost_menu_selection == static_cast<int>(i)) ? " > " : "   ";
            menu_items.push_back(ftxui::text(prefix + options[i]));
        }

        auto menu_element = ftxui::vbox(menu_items) | ftxui::border;

        if (backboost_menu_active) {
            menu_element = menu_element | ftxui::borderHeavy;
        }

        return menu_element | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 10);
    });
}

ftxui::Component Tui::render_backboost_config_input() const noexcept {
    return ftxui::Renderer([this] {
        std::vector<ftxui::Element> rows;

        // Config input fields
        rows.push_back(ftxui::text(std::format("Output Voltage (V): [{}] (5-24V)", charge_voltage_input)));
        rows.push_back(ftxui::text(std::format("Output Current (A): [{}] (0-10A)", charge_current_input)));

        rows.push_back(ftxui::text(""));
        rows.push_back(ftxui::text("Press Enter to send config"));
        rows.push_back(ftxui::text("Press Esc to cancel"));

        auto element = ftxui::vbox(rows) | ftxui::border;
        return element | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 12);
    });
}

ftxui::Component Tui::render_dac_status() const noexcept {
    return ftxui::Renderer([this] {
        auto state = connector.get_state();

        std::vector<ftxui::Element> rows;

        // DAC status
        if (state.dac_status.has_enabled) {
            rows.push_back(ftxui::text(std::format("Enabled: {}", state.dac_status.enabled ? "Yes" : "No")));
        }

        // DAC mode
        if (state.dac_status.has_mode) {
            const char* mode_str = "Unknown";
            switch (state.dac_status.mode) {
                case DACMode_DAC_MODE_VOLTAGE: mode_str = "Voltage Mode"; break;
                case DACMode_DAC_MODE_WAVEFORM: mode_str = "Waveform Mode"; break;
            }
            rows.push_back(ftxui::text(std::format("Mode: {}", mode_str)));
        }

        // Waveform type
        if (state.dac_status.has_waveform_type) {
            const char* waveform_str = "Unknown";
            switch (state.dac_status.waveform_type) {
                case WaveformType_WAVEFORM_SINE: waveform_str = "Sine"; break;
                case WaveformType_WAVEFORM_SQUARE: waveform_str = "Square"; break;
                case WaveformType_WAVEFORM_TRIANGLE: waveform_str = "Triangle"; break;
                case WaveformType_WAVEFORM_SAWTOOTH: waveform_str = "Sawtooth"; break;
                case WaveformType_WAVEFORM_CUSTOM: waveform_str = "Custom"; break;
            }
            rows.push_back(ftxui::text(std::format("Waveform: {}", waveform_str)));
        }

        // Waveform parameters
        if (state.dac_status.has_frequency) {
            rows.push_back(ftxui::text(std::format("Frequency: %.2f Hz", state.dac_status.frequency)));
        }

        if (state.dac_status.has_amplitude) {
            rows.push_back(ftxui::text(std::format("Amplitude: %.2f V", state.dac_status.amplitude)));
        }

        // Error status
        if (state.dac_status.has_error || state.dac_status.has_waveform_error) {
            if (state.dac_status.error) {
                rows.push_back(ftxui::text(std::format("Error: Yes")));
            }
            if (state.dac_status.waveform_error) {
                rows.push_back(ftxui::text(std::format("Waveform Error: Yes")));
            }
        }

        auto element = ftxui::vbox(rows) | ftxui::border;
        return element | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 20);
    });
}

ftxui::Component Tui::render_dac_menu() const noexcept {
    return ftxui::Renderer([this] {
        std::vector<ftxui::Element> menu_items;

        // Menu options
        const char* options[] = {
            "Refresh Status",
            "Refresh Measurements",
            "Refresh Config",
            "Edit Config",
            "Exit"
        };

        for (size_t i = 0; i < 5; ++i) {
            std::string prefix = (dac_menu_selection == static_cast<int>(i)) ? " > " : "   ";
            menu_items.push_back(ftxui::text(prefix + options[i]));
        }

        auto menu_element = ftxui::vbox(menu_items) | ftxui::border;

        if (dac_menu_active) {
            menu_element = menu_element | ftxui::borderHeavy;
        }

        return menu_element | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 10);
    });
}

ftxui::Component Tui::render_dac_config_input() const noexcept {
    return ftxui::Renderer([this] {
        std::vector<ftxui::Element> rows;

        // Config input fields
        rows.push_back(ftxui::text(std::format("Mode: [{}] (0=Voltage, 1=Waveform)", charge_voltage_input)));
        rows.push_back(ftxui::text(std::format("Voltage (V): [{}] (0-3.3V)", charge_current_input)));
        rows.push_back(ftxui::text(std::format("Waveform Type: [{}] (0=Sine, 1=Square, 2=Triangle, 3=Sawtooth, 4=Custom)", input_voltage_input)));
        rows.push_back(ftxui::text(std::format("Frequency (Hz): [{}] (0.1-100000)", input_current_input)));
        rows.push_back(ftxui::text(std::format("Amplitude (V): [{}] (0-3.3)", input_voltage_input)));

        rows.push_back(ftxui::text(""));
        rows.push_back(ftxui::text("Press Enter to send config"));
        rows.push_back(ftxui::text("Press Esc to cancel"));

        auto element = ftxui::vbox(rows) | ftxui::border;
        return element | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 15);
    });
}

ftxui::Component Tui::renderer() const noexcept {
    auto status_component = render_status();
    auto log_component = render_log();
    auto command_component = render_command_input();

    // Add charger status component when in charger menu
    ftxui::Components components;
    components.push_back(status_component);

    if (modal_state == ModalState::ChargerMenu) {
        auto charger_status_component = render_charger_status();
        auto charger_menu_component = render_charger_menu();
        auto charger_config_component = render_charger_config_input();

        components.push_back(charger_status_component);
        components.push_back(charger_menu_component);
        components.push_back(charger_config_component);
    } else if (modal_state == ModalState::BackBoostMenu) {
        auto backboost_status_component = render_backboost_status();
        auto backboost_menu_component = render_backboost_menu();
        auto backboost_config_component = render_backboost_config_input();

        components.push_back(backboost_status_component);
        components.push_back(backboost_menu_component);
        components.push_back(backboost_config_component);
    } else if (modal_state == ModalState::DacMenu) {
        auto dac_status_component = render_dac_status();
        auto dac_menu_component = render_dac_menu();
        auto dac_config_component = render_dac_config_input();

        components.push_back(dac_status_component);
        components.push_back(dac_menu_component);
        components.push_back(dac_config_component);
    } else {
        components.push_back(log_component);
        components.push_back(command_component);
    }

    auto root = ftxui::Container::Vertical(components);

    return root;
}

void Tui::enter_charger_menu() noexcept {
    charger_menu_active = true;
    set_modal_state(ModalState::ChargerMenu);
    
    // Initialize input fields with current config values
    if (connector.get_state().charger_config.has_charge_voltage_limit) {
        charge_voltage_input = std::to_string(connector.get_state().charger_config.charge_voltage_limit);
    }
    if (connector.get_state().charger_config.has_charge_current_limit) {
        charge_current_input = std::to_string(connector.get_state().charger_config.charge_current_limit);
    }
    if (connector.get_state().charger_config.has_input_voltage_limit) {
        input_voltage_input = std::to_string(connector.get_state().charger_config.input_voltage_limit);
    }
    if (connector.get_state().charger_config.has_input_current_limit) {
        input_current_input = std::to_string(connector.get_state().charger_config.input_current_limit);
    }
    
    add_log("Entered charger menu");
}

void Tui::exit_charger_menu() noexcept {
    charger_menu_active = false;
    charger_menu_selection = 0;
    set_modal_state(ModalState::Normal);
    add_log("Exited charger menu");
}

void Tui::send_charger_config() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_set_charger_config_tag;
    
    // Parse and set config values
    if (!charge_voltage_input.empty()) {
        req.command.set_charger_config.has_charge_voltage_limit = true;
        req.command.set_charger_config.charge_voltage_limit = std::stof(charge_voltage_input);
    }
    if (!charge_current_input.empty()) {
        req.command.set_charger_config.has_charge_current_limit = true;
        req.command.set_charger_config.charge_current_limit = std::stof(charge_current_input);
    }
    if (!input_voltage_input.empty()) {
        req.command.set_charger_config.has_input_voltage_limit = true;
        req.command.set_charger_config.input_voltage_limit = std::stof(input_voltage_input);
    }
    if (!input_current_input.empty()) {
        req.command.set_charger_config.has_input_current_limit = true;
        req.command.set_charger_config.input_current_limit = std::stof(input_current_input);
    }
    
    connector.send(req);
    add_log("Sent charger config");
    
    // Request updated config
    Request get_req = Request_init_default;
    get_req.which_command = Request_get_charger_config_tag;
    connector.send(get_req);
}

void Tui::request_charger_status() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_get_charger_status_tag;
    connector.send(req);
    add_log("Requested charger status");
}

void Tui::request_charger_measurements() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_get_charger_measurements_tag;
    connector.send(req);
    add_log("Requested charger measurements");
}

void Tui::request_charger_config() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_get_charger_config_tag;
    connector.send(req);
    add_log("Requested charger config");
}

// BackBoost menu handlers
void Tui::enter_backboost_menu() noexcept {
    backboost_menu_active = true;
    set_modal_state(ModalState::BackBoostMenu);
    add_log("Entered BackBoost menu");
}

void Tui::exit_backboost_menu() noexcept {
    backboost_menu_active = false;
    backboost_menu_selection = 0;
    set_modal_state(ModalState::Normal);
    add_log("Exited BackBoost menu");
}

void Tui::send_backboost_config() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_set_backboost_config_tag;
    connector.send(req);
    add_log("Sent BackBoost config");

    // Request updated config
    Request get_req = Request_init_default;
    get_req.which_command = Request_get_backboost_config_tag;
    connector.send(get_req);
}

void Tui::request_backboost_status() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_get_backboost_status_tag;
    connector.send(req);
    add_log("Requested BackBoost status");
}

void Tui::request_backboost_measurements() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_get_backboost_measurements_tag;
    connector.send(req);
    add_log("Requested BackBoost measurements");
}

void Tui::request_backboost_config() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_get_backboost_config_tag;
    connector.send(req);
    add_log("Requested BackBoost config");
}

// DAC menu handlers
void Tui::enter_dac_menu() noexcept {
    dac_menu_active = true;
    set_modal_state(ModalState::DacMenu);
    add_log("Entered DAC menu");
}

void Tui::exit_dac_menu() noexcept {
    dac_menu_active = false;
    dac_menu_selection = 0;
    set_modal_state(ModalState::Normal);
    add_log("Exited DAC menu");
}

void Tui::send_dac_config() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_set_dac_config_tag;
    connector.send(req);
    add_log("Sent DAC config");

    // Request updated config
    Request get_req = Request_init_default;
    get_req.which_command = Request_get_dac_config_tag;
    connector.send(get_req);
}

void Tui::request_dac_status() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_get_dac_status_tag;
    connector.send(req);
    add_log("Requested DAC status");
}

void Tui::request_dac_measurements() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_get_dac_measurements_tag;
    connector.send(req);
    add_log("Requested DAC measurements");
}

void Tui::request_dac_config() noexcept {
    Request req = Request_init_default;
    req.which_command = Request_get_dac_config_tag;
    connector.send(req);
    add_log("Requested DAC config");
}

void Tui::schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period, PeriodicTask::callback_t func) noexcept {
    const auto [ it, _ ] = tasks.emplace(std::make_pair(type, PeriodicTask(
        io_ctx, period, func, connector
    )));

    it->second.start();
}

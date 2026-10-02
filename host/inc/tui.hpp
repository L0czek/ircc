#ifndef __TUI_HPP__
#define __TUI_HPP__

#include "cli.hpp"
#include "connector.hpp"
#include "periodic_task.hpp"
#include <boost/asio/io_context.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

class Tui {
public:
    enum class BackGroundTasks {
        Keepalive
    };

    enum class ModalState {
        Normal,
        CommandInput,
        ChargerMenu,
        BackBoostMenu,
        DacMenu
    };

    enum class FocusedComponent {
        Status,
        Log,
        Command
    };

    Tui(const Options &cfg);

    void refresh() noexcept;
    void run() noexcept;

    void add_log(const std::string &line) noexcept;
    void set_modal_state(ModalState state) noexcept;
    void focus_next() noexcept;
    void focus_prev() noexcept;
    
    // Charger menu handlers
    void enter_charger_menu() noexcept;
    void exit_charger_menu() noexcept;
    void send_charger_config() noexcept;
    void request_charger_status() noexcept;
    void request_charger_measurements() noexcept;
    void request_charger_config() noexcept;

    // BackBoost menu handlers
    void enter_backboost_menu() noexcept;
    void exit_backboost_menu() noexcept;
    void send_backboost_config() noexcept;
    void request_backboost_status() noexcept;
    void request_backboost_measurements() noexcept;
    void request_backboost_config() noexcept;

    // DAC menu handlers
    void enter_dac_menu() noexcept;
    void exit_dac_menu() noexcept;
    void send_dac_config() noexcept;
    void request_dac_status() noexcept;
    void request_dac_measurements() noexcept;
    void request_dac_config() noexcept;

private:
    boost::asio::io_context io_ctx;
    Connector connector;
    ftxui::ScreenInteractive screen;
    std::unordered_map<BackGroundTasks, PeriodicTask> tasks;

    std::deque<std::string> log_buffer;
    mutable std::mutex log_mutex;
    ModalState modal_state{ModalState::Normal};
    std::string command_input;
    FocusedComponent focused{FocusedComponent::Status};
    std::time_t last_heartbeat{std::time(nullptr)};
    
    // Charger menu state
    bool charger_menu_active{false};
    int charger_menu_selection{0};

    // BackBoost menu state
    bool backboost_menu_active{false};
    int backboost_menu_selection{0};

    // DAC menu state
    bool dac_menu_active{false};
    int dac_menu_selection{0};

    // Charger config input fields
    std::string charge_voltage_input;
    std::string charge_current_input;
    std::string input_voltage_input;
    std::string input_current_input;

    ftxui::Component renderer() const noexcept;
    void schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period, PeriodicTask::callback_t func) noexcept;

    ftxui::Component render_status() const noexcept;
    ftxui::Component render_log() const noexcept;
    ftxui::Component render_command_input() const noexcept;
    ftxui::Component render_charger_status() const noexcept;
    ftxui::Component render_charger_config() const noexcept;
    ftxui::Component render_charger_config_input() const noexcept;
    ftxui::Component render_charger_menu() const noexcept;

    // BackBoost render methods
    ftxui::Component render_backboost_status() const noexcept;
    ftxui::Component render_backboost_config() const noexcept;
    ftxui::Component render_backboost_config_input() const noexcept;
    ftxui::Component render_backboost_menu() const noexcept;

    // DAC render methods
    ftxui::Component render_dac_status() const noexcept;
    ftxui::Component render_dac_config() const noexcept;
    ftxui::Component render_dac_config_input() const noexcept;
    ftxui::Component render_dac_menu() const noexcept;
};

#endif

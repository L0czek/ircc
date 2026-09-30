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
        CommandInput
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

    ftxui::Component renderer() const noexcept;
    void schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period, PeriodicTask::callback_t func) noexcept;

    ftxui::Component render_status() const noexcept;
    ftxui::Component render_log() const noexcept;
    ftxui::Component render_command_input() noexcept;
};

#endif

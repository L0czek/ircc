#ifndef __TUI_HPP__
#define __TUI_HPP__

#include "cli.hpp"
#include "connector.hpp"
#include "periodic_task.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <deque>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

// Forward declaration for CommandNode (defined in tui.cc)
struct CommandNode;

class Tui {
public:
    enum class BackGroundTasks {
        Keepalive
    };

    enum class FocusedComponent {
        Status,
        Log,
        Command
    };

    Tui(const Options &cfg);

    void refresh() noexcept;
    void run() noexcept;
    void exit() noexcept;

    void add_log(const std::string &line) noexcept;
    void focus_next() noexcept;
    void focus_prev() noexcept;

private:
    boost::asio::io_context io_ctx;
    boost::asio::steady_timer heartbeat_timer;
    Connector connector;
    ftxui::ScreenInteractive screen;
    std::unordered_map<BackGroundTasks, PeriodicTask> tasks;

    std::deque<std::string> log_buffer;
    mutable std::mutex log_mutex;
    std::string command_input;
    bool command_mode{false};
    FocusedComponent focused{FocusedComponent::Status};
    std::atomic<bool> running{false};
    std::atomic<bool> refresh_pending{false};
    std::atomic<std::time_t> last_heartbeat{std::time(nullptr)};

    // Log scrolling state
    size_t log_scroll_offset{0};
    static constexpr size_t log_max_visible{10};

    // Voltage graph display state
    static constexpr size_t voltage_graph_size{100};
    mutable std::deque<float> vbat_samples;
    mutable std::deque<float> vbus_samples;
    mutable std::deque<float> vout_samples;
    bool voltage_graph_enabled{true};

    // Signal handler support - static pointer to current instance
    static Tui* g_instance;

    ftxui::Component renderer() const noexcept;
    void schedule_heartbeat_refresh();
    void schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period, PeriodicTask::callback_t func) noexcept;

    ftxui::Component render_status() const noexcept;
    ftxui::Component render_log() const noexcept;
    ftxui::Component render_command_input() const noexcept;
    ftxui::Component render_voltage_graph() const noexcept;

    // Command system methods
    void execute_command(const std::string& command_line) noexcept;
    void show_help(const std::string& command = "") noexcept;
    void print_command_tree(const std::vector<CommandNode>& nodes, int indent = 0) noexcept;
    
    // Applet command handlers
    void handle_applet_start(const std::string& args) noexcept;
    void handle_applet_stop(const std::string& args) noexcept;
    void handle_applet_command(const std::string& args) noexcept;
    void handle_applet_list(const std::string& args) noexcept;
};

#endif

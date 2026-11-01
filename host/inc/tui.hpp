#ifndef __TUI_HPP__
#define __TUI_HPP__

#include "cli.hpp"
#include "connector.hpp"
#include "periodic_task.hpp"
#include "scope_client.hpp"
#include "trend.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief hostctl terminal UI
 *
 * Layout: tab bar (F1-F5) / active view / message strip / command line / hint line.
 * All printable keys go to the command line; Tab completes, Up/Down walk the history,
 * PgUp/PgDn/Left/Right/Home act on the active view (zoom/pan, scroll, trend window).
 *
 * Threads: the io thread (Connector callbacks: on_message, add_log) writes the shared
 * data under data_mutex; the UI thread renders from copies and owns the input state.
 * data_mutex is a leaf lock: nothing else is called while it is held.
 *
 * Sources: tui.cc (core, input), tui_views.cc (rendering), tui_commands.cc (commands,
 * forms, message handling), tui_format.cc (names, flag decoding, parsing).
 */
class Tui {
public:
    enum class Tab { Overview = 0, Power, Scope, Log, Device };
    static constexpr int kTabCount = 5;

    enum class BackGroundTasks { Keepalive, StatusPoll };

    explicit Tui(const Options &cfg);

    void refresh() noexcept;
    void run() noexcept;
    void exit() noexcept;

    /** @brief Host message (status of commands, errors); thread safe */
    void add_log(const std::string &line) noexcept;

private:
    // ---- Shared data (io thread writes, UI thread reads copies) --------------------------
    struct DeviceLogLine {
        uint32_t tick_ms;
        DeviceLogLevel level;
        std::string text;
    };
    enum TrendId { kVbus, kIbus, kVbat, kIbat, kVsys, kVac1, kVac2, kTrendCount };

    mutable std::mutex data_mutex;
    std::deque<std::string> messages;            // host messages, newest last
    size_t message_scroll{0};                    // lines up from the newest (0 = follow)
    std::deque<DeviceLogLine> device_log;        // firmware log, newest last
    std::optional<DeviceInfo> device_info;
    std::vector<std::string> applet_names;       // from the last applet list
    std::optional<ScopeConfigure> scope_config;  // last configuration sent (forms, display)
    std::array<TrendSeries, kTrendCount> trends;
    static constexpr size_t kMaxMessages = 2000;
    static constexpr size_t kMaxDeviceLog = 2000;

    // ---- Infrastructure --------------------------------------------------------------------
    Options cfg;
    boost::asio::io_context io_ctx;
    boost::asio::steady_timer heartbeat_timer;
    Connector connector;
    ScopeClient scope_client;
    std::atomic<uint32_t> requested_aux_baud{0};
    ftxui::ScreenInteractive screen;
    std::unordered_map<BackGroundTasks, PeriodicTask> tasks;
    std::atomic<bool> running{false};
    std::atomic<bool> refresh_pending{false};
    std::atomic<std::time_t> last_heartbeat{std::time(nullptr)};

    // ---- UI state (UI thread only) ---------------------------------------------------------
    std::atomic<Tab> tab{Tab::Overview};   // read by the heartbeat (io thread)
    std::string input;
    std::vector<std::string> history;
    size_t history_pos{0};          // == history.size(): editing a new line
    std::string history_draft;
    std::string hint;               // completion candidates / help for the hint line

    struct FormField {
        std::string key;            // command key (e.g. "vchg"), "" for a label-only row
        std::string label;
        std::string value;
        std::string help;
    };
    struct Form {
        std::string title;
        std::string command;        // prefix, e.g. "charger set"
        std::vector<FormField> fields;
        size_t focus{0};
    };
    std::optional<Form> form;

    // Scope view: visible window into the last capture (sample indices)
    int64_t scope_view_offset{0};
    uint64_t scope_view_length{0};  // 0 = whole capture
    bool scope_y_full{false};       // fixed 0..4095 instead of auto range
    const void* scope_view_capture{nullptr};   // capture the view refers to (reset on change)

    // Message pane: height, F6 half-screen mode, layout boxes for mouse wheel routing
    size_t message_lines{10};
    bool messages_large{false};
    mutable ftxui::Box message_box;
    void scroll_messages(int lines) noexcept;   // + = older, - = newer
    size_t message_pane_lines() const noexcept;

    // Log view
    size_t log_scroll{0};           // lines up from the newest
    DeviceLogLevel log_filter_level{DeviceLogLevel_DEVICE_LOG_TRACE};
    std::string log_filter_text;

    // Power view: trend window index into kTrendWindows
    size_t trend_window{1};
    static constexpr std::array<int, 5> kTrendWindowsS{10, 60, 300, 1800, 3600};

    // ---- tui.cc -----------------------------------------------------------------------------
    bool handle_event(ftxui::Event event) noexcept;
    bool handle_form_event(const ftxui::Event &event) noexcept;
    void handle_view_key(const ftxui::Event &event) noexcept;
    void handle_mouse(const ftxui::Event &event) noexcept;
    void complete_input() noexcept;
    void update_hint() noexcept;
    void load_history() noexcept;
    void save_history() const noexcept;
    void schedule_heartbeat_refresh();
    void schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period,
                                  PeriodicTask::callback_t func) noexcept;
    void stop_background_task(BackGroundTasks type) noexcept;
    void request_status() noexcept;
    void request_device_info() noexcept;

    // ---- tui_views.cc ----------------------------------------------------------------------
    ftxui::Element render() noexcept;
    ftxui::Element render_tab_bar(const Connector::StateSnapshot &state) const;
    ftxui::Element render_overview(const Connector::StateSnapshot &state);
    ftxui::Element render_power(const Connector::StateSnapshot &state);
    ftxui::Element render_scope(const Connector::StateSnapshot &state);
    ftxui::Element render_log();
    ftxui::Element render_device(const Connector::StateSnapshot &state);
    ftxui::Element render_form() const;
    ftxui::Element render_messages(size_t lines) const;
    ftxui::Element render_command_line() const;
    ftxui::Element charger_panel(const Connector::StateSnapshot &state) const;
    ftxui::Element output_panel(const Connector::StateSnapshot &state) const;
    ftxui::Element usb_pd_panel(const Connector::StateSnapshot &state) const;
    ftxui::Element scope_link_panel(const Connector::StateSnapshot &state) const;
    ftxui::Element trend_chart(const std::vector<const TrendSeries *> &series, int window_s) const;
    ftxui::Element scope_plot(std::shared_ptr<const ScopeClient::Capture> capture);

    // ---- tui_commands.cc -------------------------------------------------------------------
    void execute_command(const std::string &command_line) noexcept;
    void on_message(const Response &resp, frame::Type type, uint16_t seq) noexcept;
    void record_trends(const SystemStatus &status) noexcept;
    void show_help(const std::string &topic) noexcept;
    void open_form(const std::string &which) noexcept;
    void apply_form() noexcept;
    std::vector<std::string> completions(const std::vector<std::string> &words, bool trailing_space) const;

    bool handle_charger(const std::vector<std::string> &args) noexcept;
    bool handle_output(const std::vector<std::string> &args) noexcept;
    bool handle_usb(const std::vector<std::string> &args) noexcept;
    void show_usb_status() noexcept;
    bool handle_scope(const std::vector<std::string> &args) noexcept;
    void handle_link(const std::vector<std::string> &args) noexcept;
    void handle_poll(const std::vector<std::string> &args) noexcept;
    void handle_log(const std::vector<std::string> &args) noexcept;
    void handle_applet(const std::string &verb, const std::vector<std::string> &args) noexcept;
};

#endif

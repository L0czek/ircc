#include "tui.hpp"
#include "tasks.hpp"
#include "tui_format.hpp"
#include <boost/asio/post.hpp>
#include <boost/log/trivial.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace {

void sigint_handler(int) {
    auto *active_screen = ftxui::ScreenInteractive::Active();
    if (active_screen) {
        active_screen->Exit();
    }
}

std::string history_path() {
    const char *home = std::getenv("HOME");
    return home ? std::string(home) + "/.hostctl_history" : std::string();
}

constexpr size_t kMaxHistory = 500;

} // namespace

Tui::Tui(const Options &options) :
    cfg(options),
    heartbeat_timer(io_ctx),
    connector(io_ctx, [this] { refresh(); }, [this](std::string line) { add_log(line); }),
    scope_client(io_ctx, [this](const Request &req) { return connector.send(req); },
                 [this](const std::string &line) { add_log(line); }),
    screen(ftxui::ScreenInteractive::Fullscreen())
{
    trends[kVbus] = {"VBUS", "V", {}};
    trends[kIbus] = {"IBUS", "A", {}};
    trends[kVbat] = {"VBAT", "V", {}};
    trends[kIbat] = {"IBAT", "A", {}};
    trends[kVsys] = {"VSYS", "V", {}};
    trends[kVac1] = {"VAC1", "V", {}};
    trends[kVac2] = {"VAC2", "V", {}};

    connector.set_message_hook([this](const Response &resp, frame::Type type, uint16_t seq) {
        on_message(resp, type, seq);
    });
    connector.set_data_hook([this](const frame::View &view) { scope_client.on_data(view); });

    if (!connector.open(cfg, cfg.use_emulator)) {
        throw std::runtime_error("failed to open serial port");
    }
    add_log("hostctl ready - F1..F5 switch views, Tab completes, 'help' lists commands");
    load_history();

    if (cfg.keepalive) {
        schedule_background_task(BackGroundTasks::Keepalive, std::chrono::seconds(*cfg.keepalive), &keepalive_task);
    }
    // Status polling feeds the panels and the trend charts: default 1 s, --poll 0 disables.
    const std::size_t poll = cfg.status_poll.value_or(1);
    if (poll > 0) {
        schedule_background_task(BackGroundTasks::StatusPoll, std::chrono::seconds(poll), &status_poll_task);
    }

    connector.async_run_receiver();
    request_status();
    const auto level = tuifmt::parse_device_log_level(cfg.device_log);
    if (level) {
        Request req = Request_init_zero;
        req.which_command = Request_log_subscribe_tag;
        req.command.log_subscribe.has_min_level = true;
        req.command.log_subscribe.min_level = *level;   // replays the boot messages
        connector.send(req);
    }

    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
}

// =============================================================================
// Loop
// =============================================================================

void Tui::refresh() noexcept {
    if (!running.load() || refresh_pending.exchange(true)) {
        return;
    }
    screen.PostEvent(ftxui::Event::Custom);
}

void Tui::exit() noexcept {
    screen.Exit();
}

void Tui::run() noexcept {
    auto root = ftxui::Renderer([this] { return render(); });
    auto handler = ftxui::CatchEvent(root, [this](ftxui::Event event) { return handle_event(event); });

    ftxui::Loop loop(&screen, handler);
    running.store(true);
    schedule_heartbeat_refresh();
    auto work = boost::asio::make_work_guard(io_ctx);
    std::thread io_thread([this] { io_ctx.run(); });

    loop.Run();

    running.store(false);
    save_history();
    work.reset();
    io_ctx.stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    heartbeat_timer.cancel();
    connector.close();
}

void Tui::schedule_heartbeat_refresh() {
    heartbeat_timer.expires_after(1s);
    heartbeat_timer.async_wait([this](boost::system::error_code ec) {
        if (ec) {
            return;
        }
        const std::time_t now = std::time(nullptr);
        last_heartbeat.store(now);
        if (tab.load() == Tab::Device && now % 5 == 0) {
            request_device_info();   // keep stack/heap figures current while visible
        }
        refresh();
        schedule_heartbeat_refresh();
    });
}

void Tui::schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period,
                                   PeriodicTask::callback_t func) noexcept {
    boost::asio::post(io_ctx, [this, type, period, func = std::move(func)]() mutable {
        tasks.erase(type);
        auto [it, inserted] = tasks.emplace(type, PeriodicTask(io_ctx, period, std::move(func), connector));
        it->second.start();   // start in place: the timer handler captures the stored task
    });
}

void Tui::stop_background_task(BackGroundTasks type) noexcept {
    boost::asio::post(io_ctx, [this, type] {
        auto it = tasks.find(type);
        if (it != tasks.end()) {
            it->second.stop();
            tasks.erase(it);
        }
    });
}

void Tui::request_status() noexcept {
    status_poll_task(connector);
}

void Tui::request_device_info() noexcept {
    Request req = Request_init_zero;
    req.which_command = Request_get_device_info_tag;
    connector.send(req);
}

void Tui::add_log(const std::string &line) noexcept {
    {
        std::lock_guard lock(data_mutex);
        messages.push_back(line);
        if (message_scroll > 0) {
            ++message_scroll;   // scrolled up: keep the same lines in view
        }
        while (messages.size() > kMaxMessages) {
            messages.pop_front();
        }
        message_scroll = std::min(message_scroll, messages.size());
    }
    refresh();
}

// =============================================================================
// Input
// =============================================================================

bool Tui::handle_event(ftxui::Event event) noexcept {
    if (event == ftxui::Event::Custom) {
        refresh_pending.store(false);
        return true;
    }
    if (form) {
        return handle_form_event(event);
    }

    if (event.is_mouse()) {
        handle_mouse(event);
        return true;
    }
    if (event == ftxui::Event::F6) {
        messages_large = !messages_large;
        return true;
    }
    if (event == ftxui::Event::ArrowUpCtrl) {
        scroll_messages(5);
        return true;
    }
    if (event == ftxui::Event::ArrowDownCtrl) {
        scroll_messages(-5);
        return true;
    }

    static const ftxui::Event kTabKeys[kTabCount] = {ftxui::Event::F1, ftxui::Event::F2, ftxui::Event::F3,
                                                     ftxui::Event::F4, ftxui::Event::F5};
    for (int i = 0; i < kTabCount; ++i) {
        if (event == kTabKeys[i]) {
            tab = static_cast<Tab>(i);
            if (tab.load() == Tab::Device) {
                request_device_info();
            }
            return true;
        }
    }

    if (event == ftxui::Event::Return) {
        if (!input.empty()) {
            if (history.empty() || history.back() != input) {
                history.push_back(input);
                if (history.size() > kMaxHistory) {
                    history.erase(history.begin());
                }
            }
            const std::string line = input;
            input.clear();
            history_pos = history.size();
            save_history();   // every command: survives crashes and kill
            execute_command(line);
        }
        update_hint();
        return true;
    }
    if (event == ftxui::Event::Escape) {
        if (input.empty()) {
            std::lock_guard lock(data_mutex);
            message_scroll = 0;   // back to the newest messages
        }
        input.clear();
        history_pos = history.size();
        update_hint();
        return true;
    }
    if (event == ftxui::Event::Backspace) {
        if (!input.empty()) {
            input.pop_back();
        }
        update_hint();
        return true;
    }
    if (event == ftxui::Event::Tab) {
        complete_input();
        return true;
    }
    if (event == ftxui::Event::ArrowUp) {
        if (history_pos > 0) {
            if (history_pos == history.size()) {
                history_draft = input;
            }
            input = history[--history_pos];
            update_hint();
        }
        return true;
    }
    if (event == ftxui::Event::ArrowDown) {
        if (history_pos < history.size()) {
            ++history_pos;
            input = history_pos == history.size() ? history_draft : history[history_pos];
            update_hint();
        }
        return true;
    }
    if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown || event == ftxui::Event::ArrowLeft ||
        event == ftxui::Event::ArrowRight || event == ftxui::Event::Home || event == ftxui::Event::End) {
        handle_view_key(event);
        return true;
    }
    if (event.is_character()) {
        input += event.character();
        update_hint();
        return true;
    }
    return false;
}

size_t Tui::message_pane_lines() const noexcept {
    if (messages_large) {
        const int rows = ftxui::Terminal::Size().dimy;
        return std::max<size_t>(message_lines, rows > 0 ? static_cast<size_t>(rows) / 2 : 20);
    }
    return message_lines;
}

void Tui::scroll_messages(int lines) noexcept {
    std::lock_guard lock(data_mutex);
    const size_t visible = message_pane_lines();
    const size_t max_scroll = messages.size() > visible ? messages.size() - visible : 0;
    if (lines > 0) {
        message_scroll = std::min(max_scroll, message_scroll + static_cast<size_t>(lines));
    } else {
        const size_t down = static_cast<size_t>(-lines);
        message_scroll = message_scroll > down ? message_scroll - down : 0;
    }
}

void Tui::handle_mouse(const ftxui::Event &event) noexcept {
    auto m = const_cast<ftxui::Event &>(event).mouse();   // ftxui 5: mouse() is not const
    if (m.button != ftxui::Mouse::WheelUp && m.button != ftxui::Mouse::WheelDown) {
        return;
    }
    const bool up = m.button == ftxui::Mouse::WheelUp;
    if (message_box.Contain(m.x, m.y)) {
        scroll_messages(up ? 3 : -3);
        return;
    }
    switch (tab.load()) {
        case Tab::Log:
        case Tab::Overview:
            log_scroll = up ? log_scroll + 3 : (log_scroll > 3 ? log_scroll - 3 : 0);
            break;
        default:   // scope: zoom, power: trend window
            handle_view_key(up ? ftxui::Event::PageUp : ftxui::Event::PageDown);
            break;
    }
}

void Tui::handle_view_key(const ftxui::Event &event) noexcept {
    switch (tab.load()) {
        case Tab::Scope: {
            const auto capture = scope_client.last_capture();
            if (!capture || capture->count == 0) {
                return;
            }
            const uint64_t total = capture->count;
            uint64_t length = scope_view_length ? scope_view_length : total;
            int64_t offset = scope_view_offset;
            if (event == ftxui::Event::PageUp) {             // zoom in around the centre
                const uint64_t next = std::max<uint64_t>(16, length / 2);
                offset += static_cast<int64_t>((length - next) / 2);
                length = next;
            } else if (event == ftxui::Event::PageDown) {    // zoom out
                const uint64_t next = std::min<uint64_t>(total, length * 2);
                offset -= static_cast<int64_t>((next - length) / 2);
                length = next;
            } else if (event == ftxui::Event::ArrowLeft) {
                offset -= static_cast<int64_t>(std::max<uint64_t>(1, length / 8));
            } else if (event == ftxui::Event::ArrowRight) {
                offset += static_cast<int64_t>(std::max<uint64_t>(1, length / 8));
            } else if (event == ftxui::Event::Home) {
                offset = 0;
                length = total;
            } else if (event == ftxui::Event::End) {        // centre the trigger
                offset = -capture->start - static_cast<int64_t>(length / 2);
            }
            length = std::min(length, total);
            scope_view_offset = std::clamp<int64_t>(offset, 0, static_cast<int64_t>(total - length));
            scope_view_length = length >= total ? 0 : length;
            break;
        }
        case Tab::Log:
        case Tab::Overview:
            if (event == ftxui::Event::PageUp) {
                log_scroll += 10;
            } else if (event == ftxui::Event::PageDown) {
                log_scroll = log_scroll > 10 ? log_scroll - 10 : 0;
            } else if (event == ftxui::Event::End) {
                log_scroll = 0;
            } else if (event == ftxui::Event::Home) {
                log_scroll = kMaxDeviceLog;
            }
            break;
        case Tab::Power:
            if ((event == ftxui::Event::PageUp || event == ftxui::Event::ArrowLeft) && trend_window > 0) {
                --trend_window;
            } else if ((event == ftxui::Event::PageDown || event == ftxui::Event::ArrowRight) &&
                       trend_window + 1 < kTrendWindowsS.size()) {
                ++trend_window;
            }
            break;
        case Tab::Device:
            break;
    }
}

bool Tui::handle_form_event(const ftxui::Event &event) noexcept {
    Form &f = *form;
    auto move = [&f](int dir) {
        const size_t n = f.fields.size();
        for (size_t i = 0; i < n; ++i) {
            f.focus = (f.focus + n + dir) % n;
            if (!f.fields[f.focus].key.empty()) {
                break;
            }
        }
    };
    if (event == ftxui::Event::Custom) {
        refresh_pending.store(false);
        return true;
    }
    if (event == ftxui::Event::Escape) {
        form.reset();
        add_log("Form cancelled");
        return true;
    }
    if (event == ftxui::Event::Return) {
        apply_form();
        return true;
    }
    if (event == ftxui::Event::Tab || event == ftxui::Event::ArrowDown) {
        move(+1);
        return true;
    }
    if (event == ftxui::Event::TabReverse || event == ftxui::Event::ArrowUp) {
        move(-1);
        return true;
    }
    FormField &field = f.fields[f.focus];
    if (event == ftxui::Event::Backspace) {
        if (!field.value.empty()) {
            field.value.pop_back();
        }
    } else if (event == ftxui::Event::Delete) {
        field.value.clear();
    } else if (event.is_character()) {
        field.value += event.character();
    }
    return true;   // the form is modal
}

void Tui::complete_input() noexcept {
    const bool trailing_space = !input.empty() && input.back() == ' ';
    const auto words = tuifmt::split_words(input);
    const auto candidates = completions(words, trailing_space);
    if (candidates.empty()) {
        update_hint();
        return;
    }
    // Complete the last word up to the common prefix of all candidates.
    const std::string partial = (trailing_space || words.empty()) ? "" : words.back();
    std::string common = candidates.front();
    for (const auto &c : candidates) {
        size_t i = 0;
        while (i < common.size() && i < c.size() && common[i] == c[i]) {
            ++i;
        }
        common.resize(i);
    }
    if (common.size() >= partial.size() && common.starts_with(partial)) {
        input.resize(input.size() - partial.size());
        input += common;
        if (candidates.size() == 1 && !common.ends_with("=")) {
            input += ' ';
        }
    }
    update_hint();
}

void Tui::update_hint() noexcept {
    const bool trailing_space = !input.empty() && input.back() == ' ';
    const auto words = tuifmt::split_words(input);
    const auto candidates = completions(words, trailing_space);
    hint.clear();
    for (const auto &c : candidates) {
        if (hint.size() > 160) {
            hint += " ...";
            break;
        }
        hint += (hint.empty() ? "" : "  ") + c;
    }
}

void Tui::load_history() noexcept {
    const std::string path = history_path();
    if (path.empty()) {
        return;
    }
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) {
            history.push_back(line);
        }
    }
    if (history.size() > kMaxHistory) {
        history.erase(history.begin(), history.end() - kMaxHistory);
    }
    history_pos = history.size();
}

void Tui::save_history() const noexcept {
    const std::string path = history_path();
    if (path.empty()) {
        return;
    }
    std::ofstream out(path, std::ios::trunc);
    for (const auto &line : history) {
        out << line << '\n';
    }
}

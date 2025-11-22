#ifndef __TUI_HPP__
#define __TUI_HPP__

#include "cli.hpp"
#include "connector.hpp"
#include "periodic_task.hpp"
#include <boost/asio/io_context.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <unordered_map>

class Tui {
public:
    enum class BackGroundTasks {
        Keepalive
    };

    Tui(const Options &cfg);

    void refresh() noexcept;
    void run() noexcept;
private:
    boost::asio::io_context io_ctx;
    Connector connector;
    ftxui::ScreenInteractive screen;
    std::unordered_map<BackGroundTasks, PeriodicTask> tasks;

    ftxui::Component renderer() const noexcept;
    void schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period, PeriodicTask::callback_t func) noexcept;
};

#endif

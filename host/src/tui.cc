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
#include <stdexcept>
#include <thread>
#include <utility>

Tui::Tui(const Options &cfg) :
    io_ctx(),
    connector(io_ctx, std::bind(&Tui::refresh, this)),
    screen(ftxui::ScreenInteractive::FitComponent())
{
    BOOST_LOG_TRIVIAL(info) << "Connecting...\n";
    if (!connector.open(cfg)) {
        BOOST_LOG_TRIVIAL(error) << "Failed to open serial port\n";
        throw std::runtime_error("failed to open serial port");
    }

    if (cfg.keepalive) {
        schedule_background_task(
            BackGroundTasks::Keepalive,
            std::chrono::seconds(*cfg.keepalive),
            &keepalive_task
        );
    }
}

void Tui::refresh() noexcept {
    screen.PostEvent(ftxui::Event::Custom);
}

void Tui::run() noexcept {
    ftxui::Loop loop(&screen, renderer());
    auto handle = std::thread([this] { io_ctx.run(); });

    loop.Run();
    handle.join();
}

static ftxui::Component render_last_ping(const std::time_t& time) {
    std::string content;

    if (time == 0) {
        content = "Not connected";
    } else {
        content = std::format("Last ping: {}s ago", std::time(nullptr) - time);
    }

    return ftxui::Renderer([&] {
        return ftxui::text(content);
    });
}

ftxui::Component Tui::renderer() const noexcept {
    Connector::State state = connector.get_state();

    auto root = ftxui::Container::Vertical({
        render_last_ping(state.last_ping)
    });

    return root;
}

void Tui::schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period, PeriodicTask::callback_t func) noexcept {
    auto task = PeriodicTask(io_ctx, period, func, connector);
    task.start();
    tasks.insert(std::make_pair(type, std::move(task)));
}

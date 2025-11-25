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

using namespace std::chrono_literals;

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

    connector.async_run_receiver();
}

void Tui::refresh() noexcept {
    screen.PostEvent(ftxui::Event::Custom);
}

void Tui::run() noexcept {
    ftxui::Loop loop(&screen, renderer());
    auto handle = std::thread([this] { io_ctx.run(); BOOST_LOG_TRIVIAL(info) << "io service exited\n"; });

    while (true) {
        loop.RunOnceBlocking();
        screen.PostEvent(ftxui::Event::Custom);
        std::this_thread::sleep_for(100ms);
    }

    handle.join();
}

ftxui::Component Tui::render_last_ping() const noexcept {
    return ftxui::Renderer([this] {
        auto time = connector.get_state().last_ping;
        std::string content;

        if (time == 0) {
            content = "Not connected";
        } else {
            content = std::format("Last ping: {}s ago", std::time(nullptr) - time);
        }
        return ftxui::text(content) | ftxui::border;
    });
}

ftxui::Component Tui::renderer() const noexcept {
    auto root = ftxui::Container::Vertical({
        render_last_ping()
    });

    return root;
}

void Tui::schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period, PeriodicTask::callback_t func) noexcept {
    const auto [ it, _ ] = tasks.emplace(std::make_pair(type, PeriodicTask(
        io_ctx, period, func, connector
    )));

    it->second.start();
}

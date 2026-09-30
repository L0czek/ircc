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
#include <ftxui/component/container.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/cursor.hpp>
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
    screen.PostEvent(ftxui::Event::Custom);
}

void Tui::run() noexcept {
    auto root = renderer();

    ftxui::Loop loop(&screen, root);
    auto handle = std::thread([this] { 
        io_ctx.run(); 
        BOOST_LOG_TRIVIAL(info) << "io service exited\n"; 
    });

    screen.Event([&] {
        auto event = screen.GetEvent();

        if (event.Type == ftxui::Event::Type::Text) {
            if (modal_state == ModalState::CommandInput) {
                command_input += event.Text;
                return true;
            }
        }

        if (event.Type == ftxui::Event::Type::SpecialKey) {
            if (event.SpecialKey == ftxui::SpecialKey::ArrowDown) {
                focus_next();
                return true;
            }
            
            if (event.SpecialKey == ftxui::SpecialKey::ArrowUp) {
                focus_prev();
                return true;
            }

            if (modal_state == ModalState::CommandInput) {
                if (event.SpecialKey == ftxui::SpecialKey::Backspace) {
                    if (!command_input.empty()) {
                        command_input.pop_back();
                    }
                    return true;
                }

                if (event.SpecialKey == ftxui::SpecialKey::Enter) {
                    if (!command_input.empty()) {
                        add_log("Command: " + command_input);

                        if (command_input == "ping") {
                            Request req = Request_init_default;
                            req.which_command = Request_ping_tag;
                            connector.send(req);
                            add_log("Sent: ping");
                        }

                        command_input.clear();
                        set_modal_state(ModalState::Normal);
                    }
                    return true;
                }

                if (event.SpecialKey == ftxui::SpecialKey::Escape) {
                    command_input.clear();
                    set_modal_state(ModalState::Normal);
                    return true;
                }
            } else {
                if (event.Text == ":") {
                    set_modal_state(ModalState::CommandInput);
                    return true;
                }
            }
        }

        return false;
    });

    auto heartbeat_interval = std::chrono::seconds(1);

    while (true) {
        loop.RunOnceBlocking();
        screen.PostEvent(ftxui::Event::Custom);
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
                status_text = "Status: Normal | Press ':' to enter command mode | Last ping: ";
                if (connector.get_state().last_ping == 0) {
                    status_text += "Not connected";
                } else {
                    status_text += std::format("{}s ago", now - connector.get_state().last_ping);
                }
                break;
            case ModalState::CommandInput:
                status_text = "Status: Command Input | Type command and press Enter | Esc to cancel";
                break;
        }

        status_text += " | Heartbeat: " + heartbeat;

        auto element = ftxui::text(status_text) | ftxui::border;

        if (focused == FocusedComponent::Status) {
            element = element | ftxui::borderHeavy;
        }

        return element | ftxui::color("Yellow");
    });
}

ftxui::Component Tui::render_log() const noexcept {
    return ftxui::Renderer([this] {
        std::lock_guard<std::mutex> lock(log_mutex);

        auto elements = ftxui::vbox({});
        size_t count = 0;

        for (auto it = log_buffer.rbegin(); it != log_buffer.rend() && count < 20; ++it, ++count) {
            elements += ftxui::text(*it);
        }

        if (log_buffer.empty()) {
            elements += ftxui::text("No logs yet...");
        }

        auto element = elements | ftxui::border;

        if (focused == FocusedComponent::Log) {
            element = element | ftxui::borderHeavy;
        }

        return element | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 80) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 15);
    });
}

ftxui::Component Tui::render_command_input() noexcept {
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
            element = element | ftxui::cursor_invert;
        }

        element = element | ftxui::border;

        if (focused == FocusedComponent::Command) {
            element = element | ftxui::borderHeavy;
        }

        return element;
    });
}

ftxui::Component Tui::renderer() const noexcept {
    auto status_component = render_status();
    auto log_component = render_log();
    auto command_component = render_command_input();
    
    auto root = ftxui::Container::Vertical({
        status_component,
        log_component,
        command_component
    });
    
    return root;
}

void Tui::schedule_background_task(BackGroundTasks type, PeriodicTask::duration_t period, PeriodicTask::callback_t func) noexcept {
    const auto [ it, _ ] = tasks.emplace(std::make_pair(type, PeriodicTask(
        io_ctx, period, func, connector
    )));

    it->second.start();
}

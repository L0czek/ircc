#include "cli.hpp"
#include "tui.hpp"
#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>
#include <exception>
#include <csignal>

#include <boost/log/trivial.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>

using namespace boost::program_options;

// Signal handler for proper TUI exit
static volatile std::sig_atomic_t g_sigint_received = 0;

static void sigint_handler(int) {
    g_sigint_received = 1;
}

int main(int argc, char **argv) {
    boost::log::core::get()->set_filter(
        boost::log::trivial::severity >= boost::log::trivial::error
    );
    options_description desc("Allowed options");
    add_serial_options(desc);

    variables_map vm;
    try {
        store(parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            BOOST_LOG_TRIVIAL(error) << desc << "\n";
            return 0;
        }

        notify(vm);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Error: " << ex.what() << "\n";
        BOOST_LOG_TRIVIAL(error) << desc << "\n";
        return 1;
    }

    Options cfg = parse_serial_options(vm);
    boost::log::core::get()->set_filter(
        boost::log::trivial::severity >= cfg.loglevel
    );

    // Install SIGINT handler for proper Ctrl-C handling
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);

    try {
        Tui interface(cfg);
        interface.run();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Error: " << e.what() << "\n";
    }

    return 0;
}

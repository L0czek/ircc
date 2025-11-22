#include "cli.hpp"
#include "tui.hpp"
#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>
#include <exception>

using namespace boost::program_options;

int main(int argc, char **argv) {
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

    try {
        Tui interface(cfg);
        interface.run();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Error: " << e.what() << "\n";
    }

    return 0;
}

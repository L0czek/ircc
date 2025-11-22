#ifndef __PERIODIC_TASK_HPP__
#define __PERIODIC_TASK_HPP__

#include "connector.hpp"
#include <boost/asio.hpp>
#include <chrono>
#include <functional>

class PeriodicTask {
public:
    using callback_t = std::function<void(Connector &)>;
    using duration_t = std::chrono::steady_clock::duration;

    PeriodicTask(boost::asio::io_context& io,
                  duration_t period,
                  callback_t cb,
                  Connector &conn);

    void start();
    void stop();
    void set_period(duration_t d);

private:
    void schedule();

    Connector &conn;
    duration_t period;
    callback_t callback;
    boost::asio::steady_timer timer;
    bool running = false;
};

#endif

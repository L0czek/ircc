#include "periodic_task.hpp"
#include "connector.hpp"
#include <boost/log/trivial.hpp>

PeriodicTask::PeriodicTask(boost::asio::io_context& io,
                             duration_t period,
                             callback_t cb,
                             Connector &conn)
    : conn(conn),
      period(period),
      callback(std::move(cb)),
      timer(io) {}

void PeriodicTask::start() {
    if (running)
        return;
    running = true;
    schedule();
}

void PeriodicTask::stop() {
    running = false;
    boost::system::error_code ec;
    timer.cancel(ec);
}

void PeriodicTask::set_period(duration_t d) {
    period = d;
}

void PeriodicTask::schedule() {
    if (!running)
        return;

    timer.expires_after(period);
    timer.async_wait([this](boost::system::error_code ec) {
        if (ec || !running)
            return;
        BOOST_LOG_TRIVIAL(debug) << "RUNNING task\n";
        callback(conn);
        schedule();
    });
}


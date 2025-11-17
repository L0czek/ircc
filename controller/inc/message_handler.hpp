#ifndef __MESSAGE_HANDLER_HPP__
#define __MESSAGE_HANDLER_HPP__

#include "cmsis_os2.h"
#include "queue.hpp"
#include "message.hpp"
#include "ring_buffer.hpp"
#include "sem.hpp"
#include "protocol.pb.h"
#include <expected>

class MessageHandler {
    os::fixed_queue<Response, 8> tx_queue;
    os::semaphore rx_lock, tx_lock;
public:
    MessageHandler() noexcept;

    void run_sender() noexcept;
    void run_receiver() noexcept;

    void message_rx_isr() noexcept;
    void message_tx_isr() noexcept;

    std::expected<void, osStatus_t> send(Response resp) noexcept;
private:

    Response handle(const Request& req) const noexcept;
};

#endif

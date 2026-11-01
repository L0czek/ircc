#ifndef __SCOPE_SERVICE_HPP__
#define __SCOPE_SERVICE_HPP__

#include "broker.hpp"
#include "link.hpp"
#include "oscilloscope.hpp"
#include "protocol.pb.h"
#include "queue.hpp"
#include <cstdint>

/**
 * @brief Host protocol for the oscilloscope and the aux data link
 *
 * Control (protobuf, on the link the request came from): ScopeConfigure / ScopeArm /
 * ScopeForceTrigger / ScopeStop / ScopeGetStatus / ScopeRead / ScopeReadCancel,
 * DataLinkConfigure.
 *
 * Bulk data (FTP-like): ScopeRead is answered with ScopeReadStarted; after that response
 * has been sent, the pump thread reads the capture from PSRAM and sends DATA frames
 * (protocol/inc/scope_data.hpp) either on the origin link or on the aux UART, and ends
 * with a ScopeReadComplete EVENT on the origin link. One transfer at a time.
 */
class ScopeService {
public:
#if defined(OSCILLOSCOPE_SUPPORTED)
    ScopeService(Broker& broker, Oscilloscope* scope, UartLink* aux_link) noexcept;
#else
    ScopeService(Broker& broker, UartLink* aux_link) noexcept;
#endif
    ScopeService(const ScopeService&) = delete;
    ScopeService& operator=(const ScopeService&) = delete;

    /** @brief Create the data pump thread */
    void start() noexcept;

    /** @brief Handle a scope / data-link request (broker handler thread) */
    void handle(const Request& req, LinkId origin, Response& resp) noexcept;

    /** @brief The response to a request from `origin` was sent: release a pending transfer */
    void after_response(LinkId origin) noexcept;

private:
    struct Job {
        uint32_t transfer_id;
        uint32_t channel_mask;
        int64_t start;
        uint32_t count;
        LinkId data_link;
        LinkId origin;
    };

    Broker& broker_;
#if defined(OSCILLOSCOPE_SUPPORTED)
    Oscilloscope* scope_;
#endif
    UartLink* aux_;
    os::queue<Job> jobs_;
    bool job_pending_ = false;      // accepted, waiting for its ScopeReadStarted to be sent
    Job pending_{};
    volatile bool busy_ = false;    // a transfer is queued or running
    volatile uint32_t cancel_id_ = 0;
    volatile bool cancel_ = false;

    static uint8_t data_buffer_[frame::kMaxFrame];

    void data_link_configure(const DataLinkConfigure& req, Response& resp) noexcept;
#if defined(OSCILLOSCOPE_SUPPORTED)
    void configure(const ScopeConfigure& req, Response& resp) noexcept;
    void status(Response& resp) noexcept;
    void read(const ScopeRead& req, LinkId origin, Response& resp) noexcept;
    void cancel(const ScopeReadCancel& req, Response& resp) noexcept;
    static void pump_thread(ScopeService* self);
    void run_job(const Job& job) noexcept;
#endif
};

#endif

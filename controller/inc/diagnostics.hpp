#ifndef __DIAGNOSTICS_HPP__
#define __DIAGNOSTICS_HPP__

#include "broker.hpp"
#include "cmsis_os2.h"
#include "link.hpp"
#include "log.hpp"
#include "protocol.pb.h"
#include <array>
#include <atomic>
#include <cstdint>

/**
 * @brief Device diagnostics for the host: firmware log forwarding and device info
 *
 * Log forwarding: os::log messages go through a non-blocking sink into a queue; a
 * low-priority thread keeps the last kHistory messages and sends each one as a
 * LogMessage EVENT to the links that sent LogSubscribe. On subscribe the history is
 * replayed first, so boot messages are not lost. Messages are dropped (and counted)
 * when the queue is full; a link whose send fails is unsubscribed.
 *
 * Thread and queue memory is static (no FreeRTOS heap).
 */
class Diagnostics {
public:
    static constexpr size_t kQueueDepth = 8;
    static constexpr size_t kHistory = 16;
    static constexpr size_t kTextSize = 116;

    Diagnostics() noexcept;
    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;

    /** @brief Create the forwarder thread and install the log sink (call first at boot:
     *         messages logged before a host subscribes are kept in the history) */
    void start() noexcept;

    /** @brief Broker used to send LogMessage events (set before links accept requests) */
    void attach(Broker& broker) noexcept { broker_ = &broker; }

    /** @brief log_subscribe / get_device_info (broker handler thread) */
    void handle(const Request& req, LinkId origin, Response& resp) noexcept;

    /** @brief Record RCC reset flags and clear them; call once early at boot */
    static void capture_reset_flags() noexcept;

    /** @brief PSRAM state for DeviceInfo (config text is copied) */
    static void set_psram_state(bool ok, const char* config) noexcept;

private:
    struct Entry {
        uint32_t tick;
        uint8_t level;
        char text[kTextSize];
    };

    Broker* broker_ = nullptr;
    osMessageQueueId_t queue_ = nullptr;
    osThreadId_t thread_ = nullptr;
    std::array<Entry, kHistory> history_{};   // forwarder thread only
    size_t history_next_ = 0;
    size_t history_count_ = 0;
    std::atomic<uint8_t> subscribed_{0};      // bit per LinkId
    std::atomic<uint8_t> replay_{0};          // bit per LinkId: history still to send
    std::array<std::atomic<uint8_t>, kLinkCount> min_level_{};
    uint32_t reported_dropped_ = 0;

    static Diagnostics* instance_;
    static std::atomic<uint32_t> dropped_;

    static void sink(os::LogLevel level, uint32_t tick, const char* text);
    static void thread_main(void* arg);
    void run() noexcept;
    void send_entry(LinkId link, const Entry& e, uint32_t dropped) noexcept;
    void subscribe(const LogSubscribe& req, LinkId origin, Response& resp) noexcept;
    void device_info(Response& resp) noexcept;
};

#endif

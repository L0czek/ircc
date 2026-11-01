#ifndef __BROKER_HPP__
#define __BROKER_HPP__

#include "error.hpp"
#include "frame.hpp"
#include "link.hpp"
#include "mutex.hpp"
#include "protocol.pb.h"
#include "queue.hpp"
#include <array>
#include <cstdint>
#include <expected>

class MessageHandler;

/**
 * @brief Routes frames between the host links and the message handler
 *
 *   link RX ring ──► RX thread (one per request link): frame::Parser, REQUEST frames copied
 *                    into a request slot ──► handler queue
 *   handler thread ──► decode Request ──► MessageHandler::handle() ──► RESPONSE frame with
 *                    the request's seq, sent on the link the request came from
 *
 * Requests are handled one at a time (handlers never run concurrently). Any thread can
 * send RESPONSE/EVENT messages (send_message) or prebuilt frames (send_frame), e.g. the
 * oscilloscope data pump. Frame CRCs use the CRC peripheral (shared under a mutex) when it
 * is configured for CRC-32/MPEG-2 byte input, software otherwise.
 */
class Broker {
public:
    static constexpr size_t kRequestSlots = 2;
    static constexpr uint32_t kSendTimeoutMs = 1000;

    explicit Broker(MessageHandler& handler) noexcept;
    Broker(const Broker&) = delete;
    Broker& operator=(const Broker&) = delete;

    /** @brief Register a link before start(); `requests`: host requests are accepted on it */
    void add_link(Link& link, bool requests) noexcept;

    /** @brief Create the RX and handler threads */
    void start() noexcept;

    Link* link(LinkId id) const noexcept;

    /** @brief Encode and send a protobuf Response as RESPONSE (with seq) or EVENT. Thread safe. */
    std::expected<void, ControllerError> send_message(LinkId link, frame::Type type, uint16_t seq,
                                                      const Response& message) noexcept;

    /**
     * @brief Finish and send a frame whose payload is already at frame::payload_area(buffer)
     *        (buffer holds frame::kMaxFrame bytes and stays owned by the caller). Thread safe.
     */
    std::expected<void, ControllerError> send_frame(LinkId link, uint8_t* buffer, frame::Type type, uint8_t flags,
                                                    uint16_t seq, size_t payload_len) noexcept;

    /** @brief frame::CrcFn using the CRC peripheral when suitable */
    static uint32_t crc(const uint8_t* data, size_t len, void* context);

private:
    struct RxContext {
        RxContext(Broker* b, Link* l) : broker(b), link(l), parser(&Broker::crc) {}
        Broker* broker;
        Link* link;
        frame::Parser parser;   // 2 KB: constructed in place in rx_storage_
    };
    static constexpr size_t kMaxRequestLinks = 2;
    struct RequestItem {
        uint8_t slot;
        uint8_t link;
        uint16_t seq;
        uint16_t length;
    };

    MessageHandler& handler_;
    std::array<Link*, kLinkCount> links_{};
    std::array<RxContext*, kLinkCount> rx_{};
    std::array<uint16_t, kLinkCount> event_seq_{};
    os::queue<RequestItem> requests_;
    os::queue<uint8_t> free_slots_;
    os::mutex message_mutex_;   // guards message_buffer_ and event_seq_

    static std::array<std::array<uint8_t, frame::kMaxPayload>, kRequestSlots> slots_;
    static std::array<uint8_t, frame::kMaxFrame> message_buffer_;
    alignas(RxContext) static uint8_t rx_storage_[kMaxRequestLinks][sizeof(RxContext)];
    size_t rx_used_ = 0;

    static void rx_thread(RxContext* context);
    static void handler_thread(Broker* broker);
    void on_frame(RxContext& rx, const frame::View& view) noexcept;
    void handle_one(const RequestItem& item) noexcept;
    void reply_error(LinkId id, uint16_t seq, ResponseStatusCode status, ControllerError error) noexcept;
};

#endif

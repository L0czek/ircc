#include "broker.hpp"
#include "board.h"
#include "log.hpp"
#include "message_handler.hpp"
#include "pb_decode.h"
#include "pb_encode.h"
#include "thread.hpp"
#include <cstring>
#include <mutex>
#include <new>

std::array<std::array<uint8_t, frame::kMaxPayload>, Broker::kRequestSlots> Broker::slots_;
std::array<uint8_t, frame::kMaxFrame> Broker::message_buffer_;
alignas(Broker::RxContext) uint8_t Broker::rx_storage_[Broker::kMaxRequestLinks][sizeof(Broker::RxContext)];

namespace {

constexpr uint32_t kIdleMs = 20;   // no RX bytes for this long: abandon a partial frame
os::mutex& crc_mutex() {
    static os::mutex m(nullptr);   // first use in Broker constructor, before threads exist
    return m;
}

bool hardware_crc_usable() {
    const CRC_HandleTypeDef* h = BOARD_CONFIG.crc32;
    return h && h->Instance && h->Init.DefaultPolynomialUse == DEFAULT_POLYNOMIAL_ENABLE &&
           h->Init.DefaultInitValueUse == DEFAULT_INIT_VALUE_ENABLE &&
           h->Init.InputDataInversionMode == CRC_INPUTDATA_INVERSION_NONE &&
           h->Init.OutputDataInversionMode == CRC_OUTPUTDATA_INVERSION_DISABLE &&
           h->InputDataFormat == CRC_INPUTDATA_FORMAT_BYTES;
}

} // namespace

uint32_t Broker::crc(const uint8_t* data, size_t len, void*) {
    static const bool hw = hardware_crc_usable();
    if (!hw) {
        return frame::crc32_mpeg2(data, len);
    }
    std::lock_guard lock(crc_mutex());
    // Byte input format: HAL feeds the buffer byte-wise/word-wise without alignment needs.
    return HAL_CRC_Calculate(BOARD_CONFIG.crc32, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(data)),
                             static_cast<uint32_t>(len));
}

Broker::Broker(MessageHandler& handler) noexcept
    : handler_(handler),
      requests_(kRequestSlots + 1, nullptr),
      free_slots_(kRequestSlots, nullptr),
      message_mutex_(nullptr)
{
    (void)crc_mutex();
    (void)crc(nullptr, 0, nullptr);   // evaluate the hardware check before threads start
    for (uint8_t i = 0; i < kRequestSlots; ++i) {
        (void)free_slots_.put(i, 0, 0);
    }
}

void Broker::add_link(Link& link, bool requests) noexcept {
    const auto i = static_cast<size_t>(link.id());
    links_[i] = &link;
    if (requests && rx_used_ < kMaxRequestLinks) {
        rx_[i] = new (rx_storage_[rx_used_++]) RxContext(this, &link);   // in place: no 2 KB stack temporary
    }
}

Link* Broker::link(LinkId id) const noexcept {
    return links_[static_cast<size_t>(id)];
}

void Broker::start() noexcept {
    static osThreadAttr_t rx_attr[kMaxRequestLinks] = {};
    static const char* const rx_names[kMaxRequestLinks] = {"link-rx-0", "link-rx-1"};
    size_t n = 0;
    for (RxContext* rx : rx_) {
        if (!rx || n >= kMaxRequestLinks) {
            continue;
        }
        rx_attr[n] = osThreadAttr_t{};
        rx_attr[n].name = rx_names[n];
        rx_attr[n].attr_bits = osThreadDetached;
        rx_attr[n].stack_size = 1024;
        rx_attr[n].priority = osPriorityAboveNormal;   // drain the RX rings promptly
        // Threads live forever; the os::thread objects are intentionally never destroyed.
        new os::thread<void (*)(RxContext*), RxContext*>(&rx_attr[n], &Broker::rx_thread, std::move(rx));
        ++n;
    }

    static osThreadAttr_t handler_attr = {};
    handler_attr.name = "message-handler";
    handler_attr.attr_bits = osThreadDetached;
    handler_attr.stack_size = 3072;   // nanopb encode/decode, handlers, logging
    handler_attr.priority = osPriorityNormal;
    static auto handler = os::thread(&handler_attr, &Broker::handler_thread, this);
    (void)handler;
}

// =============================================================================
// Receive path
// =============================================================================

void Broker::rx_thread(RxContext* rx) {
    uint8_t chunk[128];
    for (;;) {
        const size_t n = rx->link->read(chunk, sizeof(chunk), kIdleMs);
        auto deliver = [rx](const frame::View& view) { rx->broker->on_frame(*rx, view); };
        if (n == 0) {
            if (!rx->parser.idle()) {
                rx->parser.on_idle(deliver);
            }
            continue;
        }
        rx->parser.feed(chunk, n, deliver);
    }
}

void Broker::on_frame(RxContext& rx, const frame::View& view) noexcept {
    if (view.type != frame::Type::Request) {
        return;   // the device only accepts requests
    }
    uint8_t slot = 0;
    uint8_t prio = 0;
    if (free_slots_.get(slot, prio, 0) != osOK) {
        // Handler busy with earlier requests: tell the host instead of dropping silently.
        reply_error(rx.link->id(), view.seq, ResponseStatusCode_RESPONSE_STATUS_FAILURE, ControllerError::HAL_BUSY);
        return;
    }
    std::memcpy(slots_[slot].data(), view.payload, view.length);
    const RequestItem item{slot, static_cast<uint8_t>(rx.link->id()), view.seq, static_cast<uint16_t>(view.length)};
    (void)requests_.put(item, 0, osWaitForever);
}

// =============================================================================
// Handler
// =============================================================================

void Broker::handler_thread(Broker* broker) {
    for (;;) {
        RequestItem item{};
        uint8_t prio = 0;
        if (broker->requests_.get(item, prio, osWaitForever) == osOK) {
            broker->handle_one(item);
        }
    }
}

void Broker::handle_one(const RequestItem& item) noexcept {
    static Request request;
    static Response response;
    const auto origin = static_cast<LinkId>(item.link);

    request = Request_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(slots_[item.slot].data(), item.length);
    const bool decoded = pb_decode(&stream, Request_fields, &request);
    (void)free_slots_.put(item.slot, 0, 0);
    if (!decoded) {
        os::error("Broker: undecodable request on %s: %s\n", links_[item.link]->name(), PB_GET_ERROR(&stream));
        reply_error(origin, item.seq, ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS, ControllerError::UnknownError);
        return;
    }

    response = Response_init_zero;
    handler_.handle(request, origin, response);
    if (auto r = send_message(origin, frame::Type::Response, item.seq, response); !r) {
        os::error("Broker: response on %s failed: %s\n", links_[item.link]->name(), to_string(r.error()));
    }
    handler_.after_response(origin);
}

void Broker::reply_error(LinkId id, uint16_t seq, ResponseStatusCode status, ControllerError error) noexcept {
    static Response response;
    std::lock_guard lock(message_mutex_);   // also serialises use of the static response
    response = Response_init_zero;
    response.has_result = true;
    response.result.status = status;
    response.result.has_error = true;
    response.result.error = static_cast<ControllerErrorCode>(error);
    pb_ostream_t out = pb_ostream_from_buffer(frame::payload_area(message_buffer_.data()), frame::kMaxPayload);
    if (pb_encode(&out, Response_fields, &response)) {
        if (Link* l = link(id)) {
            const size_t n = frame::finish(message_buffer_.data(), message_buffer_.size(), frame::Type::Response, 0,
                                           seq, out.bytes_written, &Broker::crc);
            (void)l->send(message_buffer_.data(), n, kSendTimeoutMs);
        }
    }
}

// =============================================================================
// Transmit
// =============================================================================

std::expected<void, ControllerError> Broker::send_message(LinkId id, frame::Type type, uint16_t seq,
                                                          const Response& message) noexcept {
    Link* l = link(id);
    if (!l) {
        return std::unexpected(ControllerError::HAL_ERROR);
    }
    std::lock_guard lock(message_mutex_);
    pb_ostream_t out = pb_ostream_from_buffer(frame::payload_area(message_buffer_.data()), frame::kMaxPayload);
    if (!pb_encode(&out, Response_fields, &message)) {
        os::error("Broker: encode failed: %s\n", PB_GET_ERROR(&out));
        return std::unexpected(ControllerError::MemoryAllocationFailed);
    }
    if (type == frame::Type::Event) {
        seq = event_seq_[static_cast<size_t>(id)]++;
    }
    const size_t n = frame::finish(message_buffer_.data(), message_buffer_.size(), type, 0, seq, out.bytes_written,
                                   &Broker::crc);
    return l->send(message_buffer_.data(), n, kSendTimeoutMs);
}

std::expected<void, ControllerError> Broker::send_frame(LinkId id, uint8_t* buffer, frame::Type type, uint8_t flags,
                                                        uint16_t seq, size_t payload_len) noexcept {
    Link* l = link(id);
    if (!l) {
        return std::unexpected(ControllerError::HAL_ERROR);
    }
    const size_t n = frame::finish(buffer, frame::kMaxFrame, type, flags, seq, payload_len, &Broker::crc);
    if (n == 0) {
        return std::unexpected(ControllerError::MemoryAllocationFailed);
    }
    return l->send(buffer, n, kSendTimeoutMs);
}

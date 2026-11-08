#ifndef __MESSAGE_HANDLER_HPP__
#define __MESSAGE_HANDLER_HPP__

#include "link.hpp"
#include "protocol.pb.h"
#include <cstdint>

class ScopeService;
class Diagnostics;
class UsbPdPort;
class PowerPath;

/**
 * @brief Executes host requests. Transport, framing and threading live in Broker;
 *        handle() is called from the broker's single handler thread.
 */
class MessageHandler {
public:
    /** @brief Oscilloscope / data-link requests are forwarded here (nullptr: unsupported) */
    void set_scope_service(ScopeService* scope) noexcept;

    /** @brief Log subscription / device info requests are forwarded here */
    void set_diagnostics(Diagnostics* diagnostics) noexcept { diagnostics_ = diagnostics; }

    /** @brief USB-C port: status in system_status, set_usb_pd_config / usb_pd_renegotiate (nullptr: unsupported) */
    void set_usb_pd(UsbPdPort* port) noexcept;

    /** @brief Power path: ship-mode power off (power_off request, system_status.power_off) */
    void set_power_path(PowerPath* power_path) noexcept;

    /** @brief Create the request lock (call once at boot, before any thread uses handle) */
    static void init() noexcept;

    /**
     * @brief Fill `resp` (zero-initialised by the caller) for `req` received on `origin`.
     *        Requests are serialised: host links and the front panel share one lock.
     */
    void handle(const Request& req, LinkId origin, Response& resp) noexcept;

    /**
     * @brief Run a request for an in-firmware client (the front panel) exactly as a host
     *        request. Link-bound requests (scope, data link, log subscription) are refused.
     */
    bool handle_local(const Request& req, Response& resp) noexcept;

    /** @brief A host request arrived in the last window_ms */
    static bool host_active(uint32_t window_ms = 5000) noexcept;

    /** @brief Called after the response to a request from `origin` has been sent */
    void after_response(LinkId origin) noexcept;

private:
    void handle_locked(const Request& req, LinkId origin, Response& resp) noexcept;

    ScopeService* scope_ = nullptr;
    Diagnostics* diagnostics_ = nullptr;
};

#endif

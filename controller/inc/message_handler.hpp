#ifndef __MESSAGE_HANDLER_HPP__
#define __MESSAGE_HANDLER_HPP__

#include "link.hpp"
#include "protocol.pb.h"

class ScopeService;
class Diagnostics;
class UsbPdPort;

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

    /** @brief Fill `resp` (zero-initialised by the caller) for `req` received on `origin` */
    void handle(const Request& req, LinkId origin, Response& resp) noexcept;

    /** @brief Called after the response to a request from `origin` has been sent */
    void after_response(LinkId origin) noexcept;

private:
    ScopeService* scope_ = nullptr;
    Diagnostics* diagnostics_ = nullptr;
};

#endif

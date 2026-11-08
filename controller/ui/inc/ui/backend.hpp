#ifndef UI_BACKEND_HPP
#define UI_BACKEND_HPP

#include "applet_control.hpp"
#include "protocol.pb.h"
#include <cstddef>
#include <cstdint>

namespace ui {

struct SystemInfo {
    const char* firmware = "";
    const char* board = "";
    uint32_t uptime_s = 0;
    bool host_connected = false;
    uint32_t free_heap = 0;
    uint32_t i2c_recoveries = 0;
    bool keypad_ok = false;
    uint8_t keypad_address = 0;
    uint32_t display_errors = 0;
};

/**
 * @brief Everything the panel reads or changes goes through here.
 *
 * Power devices use the host protocol messages: execute() runs a Request exactly as if it
 * came from the host (same validation, limits and logging), so the panel and the host
 * always agree. Applets are reached through AppletControl, start/stop through the runner.
 *
 * Firmware: front_panel.cc (MessageHandler + AppletRunner). Host: the simulator's fake.
 */
class Backend {
public:
    /** @brief Run a request; false if it could not be executed at all */
    virtual bool execute(const Request& request, Response& response) noexcept = 0;

    virtual size_t applet_count() noexcept = 0;
    virtual AppletControl* applet(size_t index) noexcept = 0;
    virtual AppletResult applet_start(size_t index) noexcept = 0;
    virtual void applet_stop(size_t index) noexcept = 0;
    virtual AppletResult applet_set(size_t index, size_t param, float value) noexcept = 0;
    /** @brief When the running applet was started (ms clock), 0 if unknown */
    virtual uint32_t applet_started_ms(size_t index) noexcept = 0;

    virtual void system_info(SystemInfo& info) noexcept = 0;

protected:
    ~Backend() = default;
};

} // namespace ui

#endif

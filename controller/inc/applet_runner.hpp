#ifndef __APPLET_RUNNER_HPP__
#define __APPLET_RUNNER_HPP__

#include "applet.hpp"
#include "applet_control.hpp"
#include "cmsis_os2.h"
#include <cstddef>
#include <cstdint>

/**
 * @brief Owns the running applet: one at a time, shared by the host requests
 *        (applet_start / applet_stop / applet_command) and the front panel.
 *
 * Starting an applet stops the one that runs. Every call holds one mutex, so parameter
 * changes from the panel and the host never interleave with start/stop. A small thread
 * ("applets", static memory) calls step() of the running applet every kStepMs.
 */
class AppletRunner {
public:
    static constexpr uint32_t kStepMs = 10;

    static AppletRunner& instance() noexcept;

    /** @brief Create the mutex and the step thread (once, after the registry is filled) */
    void start_thread() noexcept;

    size_t count() const noexcept { return AppletRegistry::load().get_count(); }
    Applet* applet(size_t index) const noexcept { return AppletRegistry::load().fetch_by_idx(static_cast<uint32_t>(index)); }
    int index_of(const Applet* applet) const noexcept;

    AppletResult start(size_t index) noexcept;
    void stop(size_t index) noexcept;
    void stop_all() noexcept;
    /** @brief Range-checked set (check_param) */
    AppletResult set_param(size_t index, size_t param, float value) noexcept;
    /** @brief Run a CLI-style configure() under the lock */
    std::expected<void, ControllerError> configure(size_t index, int argc, char** argv) noexcept;
    std::expected<void, ControllerError> on_message(size_t index, int argc, char** argv) noexcept;

    /** @brief osKernelGetTickCount() when the applet was started (0: not running) */
    uint32_t started_ms(size_t index) const noexcept;

private:
    AppletRunner() = default;
    static void thread_main(void* arg);
    void run() noexcept;

    osMutexId_t mutex_ = nullptr;
    osThreadId_t thread_ = nullptr;
    int active_ = -1;
    uint32_t started_ms_ = 0;
};

#endif

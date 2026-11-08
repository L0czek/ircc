#include "applet_runner.hpp"

#include "FreeRTOS.h"
#include "log.hpp"

namespace {

constexpr size_t kStackBytes = 1024;
alignas(8) uint8_t g_stack[kStackBytes];
StaticTask_t g_tcb;
StaticSemaphore_t g_mutex_cb;

class Guard {
public:
    explicit Guard(osMutexId_t m) noexcept : m_(m) {
        if (m_) {
            (void)osMutexAcquire(m_, osWaitForever);
        }
    }
    ~Guard() {
        if (m_) {
            (void)osMutexRelease(m_);
        }
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    osMutexId_t m_;
};

} // namespace

AppletRunner& AppletRunner::instance() noexcept {
    static AppletRunner runner;
    return runner;
}

void AppletRunner::start_thread() noexcept {
    if (mutex_ == nullptr) {
        osMutexAttr_t mattr{};
        mattr.name = "applets";
        mattr.attr_bits = osMutexPrioInherit | osMutexRecursive;
        mattr.cb_mem = &g_mutex_cb;
        mattr.cb_size = sizeof(g_mutex_cb);
        mutex_ = osMutexNew(&mattr);
    }
    if (thread_ != nullptr) {
        return;
    }
    osThreadAttr_t attr{};
    attr.name = "applets";
    attr.attr_bits = osThreadDetached;
    attr.cb_mem = &g_tcb;
    attr.cb_size = sizeof(g_tcb);
    attr.stack_mem = g_stack;
    attr.stack_size = kStackBytes;
    attr.priority = osPriorityBelowNormal;
    thread_ = osThreadNew(&AppletRunner::thread_main, this, &attr);
    if (thread_ == nullptr) {
        os::error("AppletRunner: thread creation failed\n");
    }
}

int AppletRunner::index_of(const Applet* applet) const noexcept {
    for (size_t i = 0; i < count(); ++i) {
        if (this->applet(i) == applet) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

AppletResult AppletRunner::start(size_t index) noexcept {
    Guard g(mutex_);
    Applet* a = applet(index);
    if (a == nullptr) {
        return std::unexpected(AppletError::NotFound);
    }
    if (a->run_state() == AppletRun::Running) {
        return {};
    }
    // One applet at a time
    for (size_t i = 0; i < count(); ++i) {
        Applet* other = applet(i);
        if (other && i != index && other->run_state() == AppletRun::Running) {
            os::info("applets: stopping %s to start %s\n", other->name(), a->name());
            other->stop();
        }
    }
    auto r = a->start();
    if (r) {
        active_ = static_cast<int>(index);
        started_ms_ = osKernelGetTickCount();
        if (started_ms_ == 0) {
            started_ms_ = 1;
        }
        os::info("applets: %s started\n", a->name());
    } else {
        os::error("applets: %s failed to start (%s)\n", a->name(), applet_error_text(r.error()));
    }
    return r;
}

void AppletRunner::stop(size_t index) noexcept {
    Guard g(mutex_);
    Applet* a = applet(index);
    if (a == nullptr) {
        return;
    }
    a->stop();
    if (active_ == static_cast<int>(index)) {
        active_ = -1;
        started_ms_ = 0;
    }
    os::info("applets: %s stopped\n", a->name());
}

void AppletRunner::stop_all() noexcept {
    Guard g(mutex_);
    for (size_t i = 0; i < count(); ++i) {
        Applet* a = applet(i);
        if (a && a->run_state() != AppletRun::Stopped) {
            a->stop();
        }
    }
    active_ = -1;
    started_ms_ = 0;
}

AppletResult AppletRunner::set_param(size_t index, size_t param, float value) noexcept {
    Guard g(mutex_);
    Applet* a = applet(index);
    if (a == nullptr) {
        return std::unexpected(AppletError::NotFound);
    }
    const auto params = a->params();
    if (param >= params.size()) {
        return std::unexpected(AppletError::NotFound);
    }
    const ParamDesc& d = params[param];
    if (d.flags & kParamReadOnly) {
        return std::unexpected(AppletError::Unsupported);
    }
    if (a->run_state() == AppletRun::Running && !(d.flags & kParamLive)) {
        return std::unexpected(AppletError::InvalidState);
    }
    auto checked = check_param(d, value);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    return a->set_param(param, *checked);
}

std::expected<void, ControllerError> AppletRunner::configure(size_t index, int argc, char** argv) noexcept {
    Guard g(mutex_);
    Applet* a = applet(index);
    if (a == nullptr) {
        return std::unexpected(ControllerError::Peripheral_InvalidParameter);
    }
    return a->configure(argc, argv);
}

std::expected<void, ControllerError> AppletRunner::on_message(size_t index, int argc, char** argv) noexcept {
    Guard g(mutex_);
    Applet* a = applet(index);
    if (a == nullptr) {
        return std::unexpected(ControllerError::Peripheral_InvalidParameter);
    }
    return a->on_message(argc, argv);
}

uint32_t AppletRunner::started_ms(size_t index) const noexcept {
    return active_ == static_cast<int>(index) ? started_ms_ : 0;
}

void AppletRunner::thread_main(void* arg) {
    static_cast<AppletRunner*>(arg)->run();
}

void AppletRunner::run() noexcept {
    for (;;) {
        osDelay(kStepMs);
        Guard g(mutex_);
        if (active_ < 0) {
            continue;
        }
        Applet* a = applet(static_cast<size_t>(active_));
        if (a == nullptr) {
            continue;
        }
        if (a->run_state() != AppletRun::Running) {
            // Stopped itself or faulted; the panel shows the fault
            active_ = -1;
            started_ms_ = 0;
            continue;
        }
        auto r = a->step();
        if (!r) {
            os::error("applets: %s step failed (%d), stopping\n", a->name(), static_cast<int>(r.error()));
            a->stop();
            active_ = -1;
            started_ms_ = 0;
        }
    }
}

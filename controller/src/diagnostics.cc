#include "diagnostics.hpp"
#include "FreeRTOS.h"
#include "board.h"
#include "task.h"
#include <cstdio>
#include <cstring>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif
#ifndef CONTROLLER_BOARD
#define CONTROLLER_BOARD "unknown"
#endif

Diagnostics* Diagnostics::instance_ = nullptr;
std::atomic<uint32_t> Diagnostics::dropped_{0};

namespace {

uint32_t g_reset_flags = 0;
bool g_psram_ok = false;
char g_psram_config[160] = "";

// Static memory for the forwarder thread and the log queue (no FreeRTOS heap)
constexpr uint32_t kStackBytes = 1024;
alignas(8) uint64_t g_stack[kStackBytes / 8];
StaticTask_t g_tcb;
StaticQueue_t g_queue_cb;

} // namespace

// Queue storage sized from the private Entry type
alignas(4) static uint8_t g_queue_storage[Diagnostics::kQueueDepth * (8 + Diagnostics::kTextSize)];

void Diagnostics::capture_reset_flags() noexcept {
    g_reset_flags = RCC->CSR & 0xFE000000u;   // LPWR, WWDG, IWDG, SFT, BOR, PIN, OBL
    __HAL_RCC_CLEAR_RESET_FLAGS();
}

void Diagnostics::set_psram_state(bool ok, const char* config) noexcept {
    g_psram_ok = ok;
    std::snprintf(g_psram_config, sizeof(g_psram_config), "%s", config ? config : "");
}

Diagnostics::Diagnostics() noexcept {
    for (auto& level : min_level_) {
        level = static_cast<uint8_t>(os::LogLevel::Info);
    }
}

void Diagnostics::start() noexcept {
    static_assert(sizeof(Entry) <= 8 + kTextSize, "queue storage too small");
    osMessageQueueAttr_t qattr{};
    qattr.name = "log-queue";
    qattr.cb_mem = &g_queue_cb;
    qattr.cb_size = sizeof(g_queue_cb);
    qattr.mq_mem = g_queue_storage;
    qattr.mq_size = sizeof(g_queue_storage);
    queue_ = osMessageQueueNew(kQueueDepth, sizeof(Entry), &qattr);

    osThreadAttr_t tattr{};
    tattr.name = "log-forwarder";
    tattr.attr_bits = osThreadDetached;
    tattr.cb_mem = &g_tcb;
    tattr.cb_size = sizeof(g_tcb);
    tattr.stack_mem = g_stack;
    tattr.stack_size = kStackBytes;
    tattr.priority = osPriorityBelowNormal;
    if (queue_) {
        thread_ = osThreadNew(&Diagnostics::thread_main, this, &tattr);
    }
    if (queue_ && thread_) {
        instance_ = this;
        os::set_sink(&Diagnostics::sink);
    }
}

// =============================================================================
// Log forwarding
// =============================================================================

void Diagnostics::sink(os::LogLevel level, uint32_t tick, const char* text) {
    Diagnostics* self = instance_;
    if (!self || osThreadGetId() == self->thread_) {
        return;   // never forward the forwarder's own messages (send failures would loop)
    }
    Entry e;
    e.tick = tick;
    e.level = static_cast<uint8_t>(level);
    size_t n = std::strlen(text);
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r')) {
        --n;
    }
    if (n >= sizeof(e.text)) {
        n = sizeof(e.text) - 1;
    }
    std::memcpy(e.text, text, n);
    e.text[n] = '\0';
    if (osMessageQueuePut(self->queue_, &e, 0, 0) != osOK) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Diagnostics::thread_main(void* arg) {
    static_cast<Diagnostics*>(arg)->run();
}

void Diagnostics::run() noexcept {
    for (;;) {
        Entry e;
        const bool got = osMessageQueueGet(queue_, &e, nullptr, 100) == osOK;

        // Replay the history to newly subscribed links (before the new message).
        if (const uint8_t replay = replay_.exchange(0); replay) {
            for (size_t id = 0; id < kLinkCount; ++id) {
                if (!(replay & (1u << id))) {
                    continue;
                }
                const size_t first = (history_next_ + kHistory - history_count_) % kHistory;
                for (size_t i = 0; i < history_count_; ++i) {
                    const Entry& h = history_[(first + i) % kHistory];
                    if (h.level >= min_level_[id]) {
                        send_entry(static_cast<LinkId>(id), h, 0);
                    }
                }
            }
        }
        if (!got) {
            continue;
        }

        history_[history_next_] = e;
        history_next_ = (history_next_ + 1) % kHistory;
        if (history_count_ < kHistory) {
            ++history_count_;
        }

        const uint32_t dropped = dropped_.load(std::memory_order_relaxed);
        const uint32_t newly_dropped = dropped - reported_dropped_;
        reported_dropped_ = dropped;
        const uint8_t subscribed = subscribed_.load();
        for (size_t id = 0; id < kLinkCount; ++id) {
            if ((subscribed & (1u << id)) && e.level >= min_level_[id]) {
                send_entry(static_cast<LinkId>(id), e, newly_dropped);
            }
        }
    }
}

void Diagnostics::send_entry(LinkId link, const Entry& e, uint32_t dropped) noexcept {
    static Response event;   // forwarder thread only
    event = Response_init_zero;
    event.which_payload = Response_log_message_tag;
    LogMessage& m = event.payload.log_message;
    m.tick_ms = e.tick;
    m.level = static_cast<DeviceLogLevel>(e.level);
    std::snprintf(m.text, sizeof(m.text), "%s", e.text);
    if (dropped) {
        m.has_dropped = true;
        m.dropped = dropped;
    }
    if (!broker_ || !broker_->send_message(link, frame::Type::Event, 0, event)) {
        // Host gone (USB unplugged, port closed): stop until it subscribes again.
        subscribed_.fetch_and(static_cast<uint8_t>(~(1u << static_cast<uint8_t>(link))));
    }
}

void Diagnostics::subscribe(const LogSubscribe& req, LinkId origin, Response& resp) noexcept {
    const auto id = static_cast<uint8_t>(origin);
    const uint8_t bit = static_cast<uint8_t>(1u << id);
    resp.has_result = true;
    if (!instance_) {
        resp.result.status = ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE;
        return;
    }
    if (req.enabled) {
        min_level_[id] = static_cast<uint8_t>(req.min_level);
        if (req.replay) {
            replay_.fetch_or(bit);
        }
        subscribed_.fetch_or(bit);
    } else {
        subscribed_.fetch_and(static_cast<uint8_t>(~bit));
        replay_.fetch_and(static_cast<uint8_t>(~bit));
    }
    resp.result.status = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
}

// =============================================================================
// Device info
// =============================================================================

void Diagnostics::device_info(Response& resp) noexcept {
    resp.which_payload = Response_device_info_tag;
    DeviceInfo& info = resp.payload.device_info;
    std::snprintf(info.board, sizeof(info.board), "%s", CONTROLLER_BOARD);
    std::snprintf(info.firmware, sizeof(info.firmware), "%s %s %s", FIRMWARE_VERSION, __DATE__, __TIME__);
    info.uptime_ms = HAL_GetTick();
    info.heap_free = static_cast<uint32_t>(xPortGetFreeHeapSize());
    info.heap_min_free = static_cast<uint32_t>(xPortGetMinimumEverFreeHeapSize());
    info.reset_flags = g_reset_flags;
    info.has_psram_ok = true;
    info.psram_ok = g_psram_ok;
    info.has_psram_config = g_psram_config[0] != '\0';
    std::snprintf(info.psram_config, sizeof(info.psram_config), "%s", g_psram_config);
    info.has_log_dropped = true;
    info.log_dropped = dropped_.load(std::memory_order_relaxed);

    static TaskStatus_t tasks[16];   // handler thread only
    const UBaseType_t n = uxTaskGetSystemState(tasks, 16, nullptr);   // 0 if more than 16 tasks
    constexpr size_t kMaxThreads = sizeof(info.threads) / sizeof(info.threads[0]);
    for (UBaseType_t i = 0; i < n && info.threads_count < kMaxThreads; ++i) {
        ThreadInfo& t = info.threads[info.threads_count++];
        std::snprintf(t.name, sizeof(t.name), "%s", tasks[i].pcTaskName);
        t.stack_free_bytes = static_cast<uint32_t>(tasks[i].usStackHighWaterMark) * sizeof(StackType_t);
        t.priority = static_cast<uint32_t>(tasks[i].uxCurrentPriority);
        t.state = static_cast<uint32_t>(tasks[i].eCurrentState);
    }
    resp.has_result = true;
    resp.result.status = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
}

void Diagnostics::handle(const Request& req, LinkId origin, Response& resp) noexcept {
    switch (req.which_command) {
        case Request_log_subscribe_tag:
            subscribe(req.command.log_subscribe, origin, resp);
            break;
        case Request_get_device_info_tag:
            device_info(resp);
            break;
        default:
            resp.has_result = true;
            resp.result.status = ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND;
            break;
    }
}

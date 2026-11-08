#include "scope_service.hpp"
#include "log.hpp"
#include "scope_data.hpp"
#include "thread.hpp"
#include <algorithm>
#include <bit>

alignas(4) uint8_t ScopeService::data_buffer_[frame::kMaxFrame];

namespace {

void set_result(Response& resp, ResponseStatusCode status, ControllerError error) {
    resp.has_result = true;
    resp.result.status = status;
    resp.result.has_error = error != ControllerError::HAL_OK;
    resp.result.error = static_cast<ControllerErrorCode>(error);
}

void set_error(Response& resp, ControllerError error) {
    ResponseStatusCode status = ResponseStatusCode_RESPONSE_STATUS_FAILURE;
    switch (error) {
        case ControllerError::OscilloscopeInvalidChannel:
        case ControllerError::OscilloscopeOutOfRange:
        case ControllerError::OscilloscopeBufferOverflow:
            status = ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS;
            break;
        case ControllerError::OscilloscopeNotConfigured:
        case ControllerError::OscilloscopeBufferEmpty:
        case ControllerError::PSRAM_NotInitialized:
        case ControllerError::PSRAM_Busy:
        case ControllerError::HAL_BUSY:
            status = ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE;
            break;
        case ControllerError::HAL_TIMEOUT:
            status = ResponseStatusCode_RESPONSE_STATUS_TIMEOUT;
            break;
        default:
            break;
    }
    set_result(resp, status, error);
}

void set_ok(Response& resp) {
    set_result(resp, ResponseStatusCode_RESPONSE_STATUS_SUCCESS, ControllerError::HAL_OK);
}

} // namespace

// =============================================================================
// Common
// =============================================================================

#if defined(OSCILLOSCOPE_SUPPORTED)
ScopeService::ScopeService(Broker& broker, Oscilloscope* scope, UartLink* aux_link) noexcept
    : broker_(broker), scope_(scope), aux_(aux_link), jobs_(1, nullptr)
{
}
#else
ScopeService::ScopeService(Broker& broker, UartLink* aux_link) noexcept
    : broker_(broker), aux_(aux_link), jobs_(1, nullptr)
{
}
#endif

void ScopeService::after_response(LinkId /*origin*/) noexcept {
    if (job_pending_) {
        job_pending_ = false;
        (void)jobs_.put(pending_, 0, osWaitForever);   // depth 1, busy_ guarantees it is free
    }
}

void ScopeService::data_link_configure(const DataLinkConfigure& req, Response& resp) noexcept {
    if (!aux_) {
        set_result(resp, ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND, ControllerError::HAL_OK);
        return;
    }
    if (busy_) {
        set_error(resp, ControllerError::HAL_BUSY);   // do not retune the aux UART mid-transfer
        return;
    }
    auto rate = aux_->set_baudrate(req.baudrate);
    if (!rate) {
        set_result(resp, ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS, rate.error());
        return;
    }
    resp.which_payload = Response_data_link_status_tag;
    resp.payload.data_link_status.baudrate = *rate;
    set_ok(resp);
}

#if !defined(OSCILLOSCOPE_SUPPORTED)

void ScopeService::start() noexcept {}

void ScopeService::handle(const Request& req, LinkId, Response& resp) noexcept {
    if (req.which_command == Request_data_link_configure_tag) {
        data_link_configure(req.command.data_link_configure, resp);
        return;
    }
    set_result(resp, ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND, ControllerError::HAL_OK);
}

#else

// =============================================================================
// Oscilloscope
// =============================================================================

namespace {

// Board handles; weak so boards without some of the peripherals still link (nullptr).
extern "C" {
extern ADC_HandleTypeDef hadc1 __attribute__((weak));
extern ADC_HandleTypeDef hadc2 __attribute__((weak));
extern ADC_HandleTypeDef hadc3 __attribute__((weak));
extern ADC_HandleTypeDef hadc4 __attribute__((weak));
extern ADC_HandleTypeDef hadc5 __attribute__((weak));
#if defined(HAL_COMP_MODULE_ENABLED)
extern COMP_HandleTypeDef hcomp1 __attribute__((weak));
extern COMP_HandleTypeDef hcomp2 __attribute__((weak));
extern COMP_HandleTypeDef hcomp3 __attribute__((weak));
extern COMP_HandleTypeDef hcomp4 __attribute__((weak));
extern COMP_HandleTypeDef hcomp5 __attribute__((weak));
extern COMP_HandleTypeDef hcomp6 __attribute__((weak));
extern COMP_HandleTypeDef hcomp7 __attribute__((weak));
#endif
}

ADC_HandleTypeDef* adc_handle(uint32_t number) {
    ADC_HandleTypeDef* const handles[] = {&hadc1, &hadc2, &hadc3, &hadc4, &hadc5};
    if (number < 1 || number > 5) {
        return nullptr;
    }
    ADC_HandleTypeDef* h = handles[number - 1];
    return (h && h->Instance) ? h : nullptr;
}

#if defined(HAL_COMP_MODULE_ENABLED)
COMP_HandleTypeDef* comp_handle(uint32_t number) {
    COMP_HandleTypeDef* const handles[] = {&hcomp1, &hcomp2, &hcomp3, &hcomp4, &hcomp5, &hcomp6, &hcomp7};
    if (number < 1 || number > 7) {
        return nullptr;
    }
    COMP_HandleTypeDef* h = handles[number - 1];
    return (h && h->Instance) ? h : nullptr;
}
#endif

constexpr uint32_t kSamplingTimes[] = {
    ADC_SAMPLETIME_2CYCLES_5,  ADC_SAMPLETIME_6CYCLES_5,  ADC_SAMPLETIME_12CYCLES_5,  ADC_SAMPLETIME_24CYCLES_5,
    ADC_SAMPLETIME_47CYCLES_5, ADC_SAMPLETIME_92CYCLES_5, ADC_SAMPLETIME_247CYCLES_5, ADC_SAMPLETIME_640CYCLES_5,
};

ScopeState to_proto(scope::State s) {
    switch (s) {
        case scope::State::PreTrigger:  return ScopeState_SCOPE_STATE_PRETRIGGER;
        case scope::State::PostTrigger: return ScopeState_SCOPE_STATE_POSTTRIGGER;
        case scope::State::Stopping:    return ScopeState_SCOPE_STATE_STOPPING;
        case scope::State::Done:        return ScopeState_SCOPE_STATE_DONE;
        case scope::State::Error:       return ScopeState_SCOPE_STATE_ERROR;
        case scope::State::Idle:
        default:                        return ScopeState_SCOPE_STATE_IDLE;
    }
}

} // namespace

void ScopeService::start() noexcept {
    static osThreadAttr_t attr = {};
    attr.name = "scope-pump";
    attr.attr_bits = osThreadDetached;
    attr.stack_size = 1536;
    attr.priority = osPriorityBelowNormal;   // bulk data: below request handling
    static auto pump = os::thread(&attr, &ScopeService::pump_thread, this);
    (void)pump;
}

void ScopeService::handle(const Request& req, LinkId origin, Response& resp) noexcept {
    if (!scope_ && req.which_command != Request_data_link_configure_tag) {
        set_result(resp, ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND, ControllerError::HAL_OK);
        return;
    }
    switch (req.which_command) {
        case Request_scope_configure_tag:
            configure(req.command.scope_configure, resp);
            break;
        case Request_scope_arm_tag: {
            auto r = scope_->arm();
            if (!r) {
                set_error(resp, r.error());
            } else {
                set_ok(resp);
            }
            status(resp);
            break;
        }
        case Request_scope_force_trigger_tag: {
            auto r = scope_->force_trigger();
            if (!r) {
                set_error(resp, r.error());
            } else {
                set_ok(resp);
            }
            status(resp);
            break;
        }
        case Request_scope_stop_tag: {
            auto r = scope_->stop();
            if (!r) {
                set_error(resp, r.error());
            } else {
                set_ok(resp);
            }
            status(resp);
            break;
        }
        case Request_scope_get_status_tag:
            set_ok(resp);
            status(resp);
            break;
        case Request_scope_read_tag:
            read(req.command.scope_read, origin, resp);
            break;
        case Request_scope_read_cancel_tag:
            cancel(req.command.scope_read_cancel, resp);
            break;
        case Request_data_link_configure_tag:
            data_link_configure(req.command.data_link_configure, resp);
            break;
        default:
            set_result(resp, ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND, ControllerError::HAL_OK);
            break;
    }
}

void ScopeService::configure(const ScopeConfigure& req, Response& resp) noexcept {
    if (busy_) {
        set_error(resp, ControllerError::HAL_BUSY);   // a transfer is reading the current capture
        return;
    }
    ScopeConfig cfg{};
    if (req.channels_count == 0 || req.channels_count > scope::kMaxStreams) {
        set_error(resp, ControllerError::OscilloscopeInvalidChannel);
        return;
    }
    for (pb_size_t i = 0; i < req.channels_count; ++i) {
        const ScopeChannelConfig& c = req.channels[i];
        ADC_HandleTypeDef* adc = adc_handle(c.adc);
        const uint32_t st = c.has_sampling_time ? c.sampling_time : 1u;
        if (!adc || c.input > 18 || st >= std::size(kSamplingTimes)) {
            set_error(resp, ControllerError::OscilloscopeInvalidChannel);
            return;
        }
        cfg.channels[i].adc = adc;
        cfg.channels[i].channel = __LL_ADC_DECIMAL_NB_TO_CHANNEL(c.input);
        cfg.channels[i].sampling_time = kSamplingTimes[st];
    }
    cfg.num_channels = req.channels_count;
    cfg.sample_rate_hz = req.sample_rate_hz;
    cfg.pre_trigger_samples = req.pre_trigger_samples;
    cfg.post_trigger_samples = req.post_trigger_samples;
    if (req.has_trigger) {
        const ScopeTriggerSettings& t = req.trigger;
        cfg.trigger.software = t.software;
        cfg.trigger.analog_watchdog = t.analog_watchdog;
        cfg.trigger.awd_stream = t.awd_channel;
        cfg.trigger.awd_low = static_cast<uint16_t>(std::min<uint32_t>(t.awd_low, 0xFFFF));
        cfg.trigger.awd_high = static_cast<uint16_t>(std::min<uint32_t>(t.awd_high, 0xFFFF));
#if defined(HAL_COMP_MODULE_ENABLED)
        if (t.comparator != 0) {
            cfg.trigger.comparator = comp_handle(t.comparator);
            if (!cfg.trigger.comparator) {
                set_error(resp, ControllerError::OscilloscopeNotConfigured);
                return;
            }
        }
#endif
        cfg.trigger.external = t.external;
        cfg.trigger.require_full_pretrigger = t.require_full_pretrigger;
    }
    auto r = scope_->configure(cfg);
    if (!r) {
        set_error(resp, r.error());
    } else {
        set_ok(resp);
    }
    status(resp);
}

void ScopeService::status(Response& resp) noexcept {
    scope::State state = scope_->state();
    if (state == scope::State::Done || state == scope::State::Error) {
        (void)scope_->wait_done(0);   // release the capture hardware (no wait: already finished)
        state = scope_->state();
    }
    resp.which_payload = Response_scope_status_tag;
    ScopeStatus& st = resp.payload.scope_status;
    st = ScopeStatus_init_zero;
    st.state = to_proto(state);
    auto info = scope_->info();
    if (state == scope::State::Error && !info) {
        st.has_error = true;
        st.error = static_cast<ControllerErrorCode>(info.error());
    }
    if (state == scope::State::Done && info) {
        st.has_num_channels = true;
        st.num_channels = info->num_channels;
        st.has_sample_rate_hz = true;
        st.sample_rate_hz = info->sample_rate_hz;
        st.has_trigger_source = true;
        st.trigger_source = static_cast<ScopeTriggerKind>(info->trigger_source);
        st.has_first_sample = true;
        st.first_sample = static_cast<int64_t>(info->first_index) - static_cast<int64_t>(info->trigger_index);
        st.has_post_trigger_samples = true;
        st.post_trigger_samples = static_cast<uint32_t>(info->end_index - info->trigger_index);
    }
}

void ScopeService::read(const ScopeRead& req, LinkId origin, Response& resp) noexcept {
    if (busy_) {
        set_error(resp, ControllerError::HAL_BUSY);
        return;
    }
    if (scope_->state() == scope::State::Done) {
        (void)scope_->wait_done(0);
    }
    auto info = scope_->info();
    if (!info) {
        set_error(resp, info.error());
        return;
    }
    const uint32_t all = (1u << info->num_channels) - 1u;
    if (req.channel_mask == 0 || (req.channel_mask & ~all) != 0) {
        set_error(resp, ControllerError::OscilloscopeInvalidChannel);
        return;
    }
    const int64_t first = static_cast<int64_t>(info->trigger_index) + req.start;
    if (req.count == 0 || first < static_cast<int64_t>(info->first_index) ||
        first + req.count > static_cast<int64_t>(info->end_index)) {
        set_error(resp, ControllerError::OscilloscopeOutOfRange);
        return;
    }
    LinkId data_link = origin;
    if (req.has_link && req.link == DataLink_DATA_LINK_AUX) {
        if (!aux_) {
            set_result(resp, ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND, ControllerError::HAL_OK);
            return;
        }
        data_link = aux_->id();
    }

    const uint32_t blocks = (req.count + scope_data::kSamplesPerFrame - 1u) / scope_data::kSamplesPerFrame;
    resp.which_payload = Response_scope_read_started_tag;
    ScopeReadStarted& started = resp.payload.scope_read_started;
    started.transfer_id = req.transfer_id;
    started.frames = blocks * static_cast<uint32_t>(std::popcount(req.channel_mask));
    started.samples_per_frame = scope_data::kSamplesPerFrame;
    started.link = (data_link == origin) ? DataLink_DATA_LINK_ORIGIN : DataLink_DATA_LINK_AUX;
    set_ok(resp);

    // Started after ScopeReadStarted has gone out (after_response), so it precedes the data.
    pending_ = Job{req.transfer_id, req.channel_mask, req.start, req.count, data_link, origin};
    job_pending_ = true;
    cancel_ = false;
    busy_ = true;
}

void ScopeService::cancel(const ScopeReadCancel& req, Response& resp) noexcept {
    if (busy_) {
        cancel_id_ = req.transfer_id;
        cancel_ = true;
    }
    set_ok(resp);
}

void ScopeService::pump_thread(ScopeService* self) {
    for (;;) {
        Job job{};
        uint8_t prio = 0;
        if (self->jobs_.get(job, prio, osWaitForever) == osOK) {
            self->run_job(job);
            self->busy_ = false;
        }
    }
}

void ScopeService::run_job(const Job& job) noexcept {
    const uint32_t k = scope_data::kSamplesPerFrame;
    const uint32_t blocks = (job.count + k - 1u) / k;
    uint32_t sent = 0;
    bool cancelled = false;
    ControllerError error = ControllerError::HAL_OK;

    for (uint32_t b = 0; b < blocks && error == ControllerError::HAL_OK && !cancelled; ++b) {
        const uint32_t offset = b * k;
        const uint32_t n = std::min(k, job.count - offset);
        for (uint32_t ch = 0; ch < scope::kMaxStreams; ++ch) {
            if ((job.channel_mask & (1u << ch)) == 0) {
                continue;
            }
            if (cancel_ && cancel_id_ == job.transfer_id) {
                cancelled = true;
                break;
            }
            uint8_t* payload = frame::payload_area(data_buffer_);
            auto* samples = reinterpret_cast<uint16_t*>(payload + scope_data::kHeaderSize);
            auto read_result = scope_->read(ch, job.start + offset, samples, n);
            if (!read_result) {
                error = read_result.error();
                break;
            }
            scope_data::write_header(payload, {static_cast<uint16_t>(job.transfer_id), static_cast<uint8_t>(ch),
                                               offset, static_cast<uint16_t>(n)});
            const uint8_t flags = (b + 1u == blocks) ? scope_data::kFlagLastOfChannel : 0u;
            auto send_result = broker_.send_frame(job.data_link, data_buffer_, frame::Type::Data, flags,
                                                  static_cast<uint16_t>(sent), scope_data::kHeaderSize + 2u * n);
            if (!send_result) {
                error = send_result.error();
                break;
            }
            ++sent;
        }
    }

    static Response event;
    event = Response_init_zero;
    event.which_payload = Response_scope_read_complete_tag;
    ScopeReadComplete& done = event.payload.scope_read_complete;
    done.transfer_id = job.transfer_id;
    done.frames_sent = sent;
    done.cancelled = cancelled;
    done.has_error = error != ControllerError::HAL_OK;
    done.error = static_cast<ControllerErrorCode>(error);
    auto r = broker_.send_message(job.origin, frame::Type::Event, 0, event);
    if (!r) {
        os::error("Scope: transfer %lu complete event failed\n", static_cast<unsigned long>(job.transfer_id));
    }
    cancel_ = false;
}

#endif // OSCILLOSCOPE_SUPPORTED

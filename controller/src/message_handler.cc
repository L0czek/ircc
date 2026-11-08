#include "message_handler.hpp"
#include "cmsis_os2.h"
#include "log.hpp"
#include "scope_service.hpp"
#include "diagnostics.hpp"
#include "usbpd_app.hpp"
#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "protocol.pb.h"
#include "board.h"
#include "stm32g4xx_hal_def.h"
#include "charger.hpp"
#include "output_voltage_regulator.hpp"
#include "applet.hpp"
#include "applet_runner.hpp"
#include "response_codes.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>

// Global instances - initialized in ctl.cc
extern Charger* charger;
extern OutputVoltageRegulator* output_voltage_regulator;

namespace {

ResponseStatusCode status_for(ControllerError ec) {
    switch (ec) {
        case ControllerError::BQ25792_InvalidParameter:
        case ControllerError::BQ25792_InvalidAddress:
        case ControllerError::TPS55289_InvalidParameter:
        case ControllerError::TPS55289_InvalidAddress:
        case ControllerError::DAC_INVALID_PARAMETER:
        case ControllerError::DAC_INVALID_MODE:
        case ControllerError::MCP44XX_InvalidParameter:
        case ControllerError::MCP44XX_InvalidWiper:
        case ControllerError::MCP44XX_InvalidSlot:
        case ControllerError::PSRAM_InvalidAddress:
        case ControllerError::PSRAM_InvalidSize:
        case ControllerError::OscilloscopeInvalidChannel:
        case ControllerError::OscilloscopeOutOfRange:
        case ControllerError::Peripheral_InvalidParameter:
        case ControllerError::USBPD_InvalidParameter:
            return ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS;
        case ControllerError::HAL_TIMEOUT:
        case ControllerError::BQ25792_Timeout:
        case ControllerError::TPS55289_Timeout:
        case ControllerError::DAC_TIMEOUT:
        case ControllerError::MCP44XX_Timeout:
        case ControllerError::PSRAM_Timeout:
            return ResponseStatusCode_RESPONSE_STATUS_TIMEOUT;
        case ControllerError::BQ25792_NotInitialized:
        case ControllerError::TPS55289_NotInitialized:
        case ControllerError::DAC_NOT_INITIALIZED:
        case ControllerError::PSRAM_NotInitialized:
        case ControllerError::PSRAM_Busy:
        case ControllerError::OscilloscopeNotConfigured:
        case ControllerError::USBPD_NotAttached:
        case ControllerError::USBPD_StackBusy:
            return ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE;
        case ControllerError::MemoryAllocationFailed:
            return ResponseStatusCode_RESPONSE_STATUS_NO_MEMORY;
        default:
            return ResponseStatusCode_RESPONSE_STATUS_FAILURE;
    }
}

// Collects the first error of a command and reports it in Response.result.
class CommandOutcome {
public:
    template <typename T>
    void track(const std::expected<T, ControllerError>& r) noexcept {
        if (!r && !failed_) {
            failed_ = true;
            error_ = r.error();
        }
    }

    void fill(Response& resp) const noexcept {
        resp.has_result = true;
        resp.result.status = failed_ ? status_for(error_) : ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
        resp.result.has_error = failed_;
        resp.result.error = static_cast<ControllerErrorCode>(error_);
    }

private:
    bool failed_ = false;
    ControllerError error_ = ControllerError::HAL_OK;
};

// Status, measurements and configuration of every power part. A part that is absent or
// cannot be read has its has_* flags cleared; the first error goes to resp.result.
UsbPdPort* g_usb_pd = nullptr;   // MessageHandler::set_usb_pd (resonant board only)

void fill_system_status(SystemStatus& status, CommandOutcome& outcome) noexcept {
#if defined(resonant_converter_controller)
    if (g_usb_pd) {
        // Not tracked: the port state is in RAM and cannot fail
        status.has_usb_pd = true;
        g_usb_pd->fill_status(status.usb_pd);
        status.has_usb_pd_config = true;
        g_usb_pd->fill_config(status.usb_pd_config);
    }
#endif
    if (charger) {
        outcome.track(charger->get_status(status));
        outcome.track(charger->get_measurements(status));
        outcome.track(charger->get_config(status));
    }
    if (output_voltage_regulator) {
        outcome.track(output_voltage_regulator->get_status(status));
        outcome.track(output_voltage_regulator->get_measurements(status));
        outcome.track(output_voltage_regulator->get_config(status));
    }
}

// ---- Applet parameter commands (applet_control.hpp) ----

ResponseStatusCode status_for(const AppletParamError& error) noexcept {
    return error.kind == AppletParamError::Kind::UnknownParam ? ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND
                                                              : ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS;
}

ResponseStatusCode status_for(AppletError error) noexcept {
    return error == AppletError::OutOfRange ? ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS
                                            : ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE;
}

// `applet_command <name> params | get [key] | set <key> <value>` (argv[0] = applet name):
// parse, apply a set through the runner (the only action), then write the reply.
// Returns false when argv[1] is not one of these verbs (the applet's own command).
bool run_applet_param_command(AppletRunner& runner, size_t index, int argc, char** argv, AppletStatus& st) noexcept {
    const Applet& applet = *runner.applet(index);
    const auto cmd = AppletParamCommand::parse(applet, static_cast<size_t>(argc - 1), argv + 1);
    if (!cmd && cmd.error().kind == AppletParamError::Kind::NotParamCommand) {
        return false;
    }
    AppletParamText reply(st.text, sizeof(st.text));
    if (!cmd) {
        st.status_code = status_for(cmd.error());
        reply.write_parse_error(cmd.error());
        return true;
    }
    if (cmd->verb == AppletParamCommand::Verb::Set) {
        const auto set = runner.set_param(index, static_cast<size_t>(cmd->param), cmd->value);
        if (!set) {
            st.status_code = status_for(set.error());
            reply.write_set_error(applet, *cmd, set.error());
            return true;
        }
    }
    st.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
    reply.write_reply(applet, *cmd);
    return true;
}

void not_present(Response& resp) noexcept {
    resp.has_result = true;
    resp.result.status = ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND;
}

} // namespace

void MessageHandler::set_usb_pd(UsbPdPort* port) noexcept {
    g_usb_pd = port;
}

void MessageHandler::set_scope_service(ScopeService* scope) noexcept {
    scope_ = scope;
}

void MessageHandler::after_response(LinkId origin) noexcept {
    if (scope_) {
        scope_->after_response(origin);
    }
}

void MessageHandler::handle(const Request &req, LinkId origin, Response &resp) noexcept {
    switch (req.which_command) {
        case Request_scope_configure_tag:
        case Request_scope_arm_tag:
        case Request_scope_force_trigger_tag:
        case Request_scope_stop_tag:
        case Request_scope_get_status_tag:
        case Request_scope_read_tag:
        case Request_scope_read_cancel_tag:
        case Request_data_link_configure_tag:
            if (scope_) {
                scope_->handle(req, origin, resp);
            } else {
                resp.has_result = true;
                resp.result.status = ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND;
            }
            break;

        case Request_log_subscribe_tag:
        case Request_get_device_info_tag:
            if (diagnostics_) {
                diagnostics_->handle(req, origin, resp);
            } else {
                not_present(resp);
            }
            break;

        case Request_ping_tag:
            os::debug("Received ping command\n");
            resp.which_payload = Response_pong_tag;
            break;

        case Request_get_system_status_tag: {
            os::debug("Received get system status command\n");
            resp.which_payload = Response_system_status_tag;
            CommandOutcome outcome;
            fill_system_status(resp.payload.system_status, outcome);
            outcome.fill(resp);
            break;
        }

        case Request_set_charger_config_tag: {
            os::debug("Received set charger config command\n");
            if (!charger) {
                not_present(resp);
                break;
            }
            CommandOutcome outcome;
            outcome.track(charger->set_config(req.command.set_charger_config));
            // Return the updated state
            resp.which_payload = Response_system_status_tag;
            fill_system_status(resp.payload.system_status, outcome);
            outcome.fill(resp);
            break;
        }

        case Request_set_usb_pd_config_tag:
        case Request_usb_pd_renegotiate_tag: {
#if defined(resonant_converter_controller)
            if (g_usb_pd) {
                CommandOutcome outcome;
                if (req.which_command == Request_set_usb_pd_config_tag) {
                    outcome.track(g_usb_pd->set_config(req.command.set_usb_pd_config));
                } else {
                    outcome.track(g_usb_pd->renegotiate(req.command.usb_pd_renegotiate));
                }
                resp.which_payload = Response_system_status_tag;
                fill_system_status(resp.payload.system_status, outcome);
                outcome.fill(resp);
                break;
            }
#endif
            not_present(resp);
            break;
        }

        case Request_set_output_regulator_config_tag: {
            os::debug("Received set output regulator config command\n");
            if (!output_voltage_regulator) {
                not_present(resp);
                break;
            }
            CommandOutcome outcome;
            outcome.track(output_voltage_regulator->set_config(req.command.set_output_regulator_config));
            // Return the updated state
            resp.which_payload = Response_system_status_tag;
            fill_system_status(resp.payload.system_status, outcome);
            outcome.fill(resp);
            break;
        }

        case Request_set_dac_config_tag:
            // The DAC controller has been removed.
            os::debug("Received set dac config command (unsupported)\n");
            resp.which_payload = Response_system_status_tag;
            resp.has_result = true;
            resp.result.status = ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND;
            break;

        case Request_applet_start_tag: {
            os::debug("Received applet start command\n");
            resp.which_payload = Response_applet_status_tag;
            auto& st = resp.payload.applet_status;
            AppletRunner& runner = AppletRunner::instance();
            const int index = runner.index_of(AppletRegistry::load().fetch_by_name(req.command.applet_start.applet_name));
            if (index < 0) {
                st.status_code = ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND;
                std::snprintf(st.text, sizeof(st.text), "applet not found: %s", req.command.applet_start.applet_name);
                break;
            }
            st.exists = true;
            // Arguments (if any) go to configure() first, then the applet is started
            if (req.command.applet_start.args_count > 0) {
                std::array<char*, 16> argv{};
                argv[0] = const_cast<char*>(req.command.applet_start.applet_name);
                for (pb_size_t i = 0; i < req.command.applet_start.args_count && i < 15; ++i) {
                    argv[i + 1] = const_cast<char*>(req.command.applet_start.args[i]);
                }
                const int argc = static_cast<int>(req.command.applet_start.args_count) + 1;
                auto configured = runner.configure(static_cast<size_t>(index), argc, argv.data());
                if (!configured) {
                    st.status_code = ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS;
                    std::snprintf(st.text, sizeof(st.text), "%s configure failed", req.command.applet_start.applet_name);
                    break;
                }
            }
            auto started = runner.start(static_cast<size_t>(index));
            st.running = started.has_value();
            st.status_code = started ? ResponseStatusCode_RESPONSE_STATUS_SUCCESS : ResponseStatusCode_RESPONSE_STATUS_FAILURE;
            std::snprintf(st.text, sizeof(st.text), "%s %s", req.command.applet_start.applet_name,
                          started ? "started" : applet_error_text(started.error()));
            break;
        }

        case Request_applet_stop_tag: {
            os::debug("Received applet stop command\n");
            resp.which_payload = Response_applet_status_tag;
            auto& st = resp.payload.applet_status;
            AppletRunner& runner = AppletRunner::instance();
            const int index = runner.index_of(AppletRegistry::load().fetch_by_name(req.command.applet_stop.applet_name));
            if (index < 0) {
                st.status_code = ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND;
                std::snprintf(st.text, sizeof(st.text), "applet not found: %s", req.command.applet_stop.applet_name);
                break;
            }
            runner.stop(static_cast<size_t>(index));
            st.exists = true;
            st.running = false;
            st.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
            std::snprintf(st.text, sizeof(st.text), "%s stopped", req.command.applet_stop.applet_name);
            break;
        }

        case Request_applet_command_tag: {
            os::debug("Received applet command\n");
            resp.which_payload = Response_applet_status_tag;
            auto& st = resp.payload.applet_status;
            const auto& cmd = req.command.applet_command;
            AppletRunner& runner = AppletRunner::instance();
            const int index = runner.index_of(AppletRegistry::load().fetch_by_name(cmd.applet_name));
            if (index < 0) {
                st.status_code = ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND;
                std::strncpy(st.text, response_status_code_to_string(ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND), sizeof(st.text) - 1);
                break;
            }
            Applet* applet = runner.applet(static_cast<size_t>(index));
            st.exists = true;
            std::array<char*, 16> argv{};
            argv[0] = const_cast<char*>(cmd.applet_name);
            for (pb_size_t i = 0; i < cmd.args_count && i < 15; ++i) {
                argv[i + 1] = const_cast<char*>(cmd.args[i]);
            }
            const int argc = static_cast<int>(std::min<pb_size_t>(cmd.args_count, 15)) + 1;
            // Generic parameter commands first: params | get [key] | set <key> <value>
            if (run_applet_param_command(runner, static_cast<size_t>(index), argc, argv.data(), st)) {
                st.running = applet->run_state() == AppletRun::Running;
                break;
            }
            auto result = runner.on_message(static_cast<size_t>(index), argc, argv.data());
            st.running = applet->run_state() == AppletRun::Running;
            st.status_code = result ? ResponseStatusCode_RESPONSE_STATUS_SUCCESS : ResponseStatusCode_RESPONSE_STATUS_FAILURE;
            std::strncpy(st.text, response_status_code_to_string(st.status_code), sizeof(st.text) - 1);
            break;
        }

        case Request_applet_list_tag: {
            os::debug("Received applet list command\n");
            resp.which_payload = Response_applet_list_response_tag;
            resp.payload.applet_list_response.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
            
            const auto& applets = AppletRegistry::load().get_applets();
            uint32_t count = AppletRegistry::load().get_count();
            for (pb_size_t i = 0; i < count && i < 16; ++i) {
                Applet* a = applets[i];
                if (a) {
                    std::strncpy(resp.payload.applet_list_response.applet_names[i], a->name(), sizeof(resp.payload.applet_list_response.applet_names[i]) - 1);
                    resp.payload.applet_list_response.applet_names_count++;
                }
            }
            // Text field left empty - host will format the display string
            resp.payload.applet_list_response.text[0] = '\0';
            break;
        }
    }

}

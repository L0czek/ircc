#include "message_handler.hpp"
#include "cmsis_os2.h"
#include "log.hpp"
#include "scope_service.hpp"
#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "protocol.pb.h"
#include "board.h"
#include "stm32g4xx_hal_def.h"
#include "charger.hpp"
#include "output_voltage_regulator.hpp"
#include "applet.hpp"
#include "response_codes.hpp"
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
void fill_system_status(SystemStatus& status, CommandOutcome& outcome) noexcept {
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

void not_present(Response& resp) noexcept {
    resp.has_result = true;
    resp.result.status = ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND;
}

} // namespace

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
            resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_FAILURE;  // Default to failure

            Applet* applet = AppletRegistry::load().fetch_by_name(req.command.applet_start.applet_name);
            if (applet) {
                // Parse args for configure()
                std::array<char*, 16> argv;
                argv[0] = const_cast<char*>(req.command.applet_start.applet_name);
                for (pb_size_t i = 0; i < req.command.applet_start.args_count && i < 15; ++i) {
                    argv[i + 1] = const_cast<char*>(req.command.applet_start.args[i]);
                }
                int argc = req.command.applet_start.args_count + 1;

                auto result = applet->configure(argc, argv.data());
                if (result) {
                    resp.payload.applet_status.exists = true;
                    resp.payload.applet_status.running = true;
                    resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
                    std::snprintf(resp.payload.applet_status.text, sizeof(resp.payload.applet_status.text), "%s started", req.command.applet_start.applet_name);
                } else {
                    resp.payload.applet_status.exists = true;
                    resp.payload.applet_status.running = false;
                    resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_FAILURE;
                    std::snprintf(resp.payload.applet_status.text, sizeof(resp.payload.applet_status.text), "%s configure failed", req.command.applet_start.applet_name);
                }
            } else {
                resp.payload.applet_status.exists = false;
                resp.payload.applet_status.running = false;
                resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND;
                std::snprintf(resp.payload.applet_status.text, sizeof(resp.payload.applet_status.text), "applet not found: %s", req.command.applet_start.applet_name);
            }
            break;
        }

        case Request_applet_stop_tag: {
            os::debug("Received applet stop command\n");
            resp.which_payload = Response_applet_status_tag;
            resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_FAILURE;  // Default to failure

            Applet* applet = AppletRegistry::load().fetch_by_name(req.command.applet_stop.applet_name);
            if (applet) {
                applet->shutdown();
                resp.payload.applet_status.exists = true;
                resp.payload.applet_status.running = false;
                resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
                std::snprintf(resp.payload.applet_status.text, sizeof(resp.payload.applet_status.text), "%s stopped", req.command.applet_stop.applet_name);
            } else {
                resp.payload.applet_status.exists = false;
                resp.payload.applet_status.running = false;
                resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND;
                std::snprintf(resp.payload.applet_status.text, sizeof(resp.payload.applet_status.text), "applet not found: %s", req.command.applet_stop.applet_name);
            }
            break;
        }

        case Request_applet_command_tag: {
            os::debug("Received applet command\n");
            resp.which_payload = Response_applet_status_tag;
            resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_FAILURE;  // Default to failure
            
            Applet* applet = AppletRegistry::load().fetch_by_name(req.command.applet_command.applet_name);
            if (applet) {
                // Parse args for on_message()
                std::array<char*, 16> argv;
                argv[0] = const_cast<char*>(req.command.applet_command.applet_name);
                for (pb_size_t i = 0; i < req.command.applet_command.args_count && i < 15; ++i) {
                    argv[i + 1] = const_cast<char*>(req.command.applet_command.args[i]);
                }
                int argc = req.command.applet_command.args_count + 1;

                auto result = applet->on_message(argc, argv.data());
                if (result) {
                    resp.payload.applet_status.exists = true;
                    resp.payload.applet_status.running = true;
                    resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_SUCCESS;
                    std::strncpy(resp.payload.applet_status.text, response_status_code_to_string(ResponseStatusCode_RESPONSE_STATUS_SUCCESS), sizeof(resp.payload.applet_status.text) - 1);
                } else {
                    resp.payload.applet_status.exists = true;
                    resp.payload.applet_status.running = true;
                    resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_FAILURE;
                    std::strncpy(resp.payload.applet_status.text, response_status_code_to_string(ResponseStatusCode_RESPONSE_STATUS_FAILURE), sizeof(resp.payload.applet_status.text) - 1);
                }
            } else {
                resp.payload.applet_status.exists = false;
                resp.payload.applet_status.running = false;
                resp.payload.applet_status.status_code = ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND;
                std::strncpy(resp.payload.applet_status.text, response_status_code_to_string(ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND), sizeof(resp.payload.applet_status.text) - 1);
            }
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

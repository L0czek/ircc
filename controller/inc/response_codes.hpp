#ifndef __RESPONSE_CODES_HPP__
#define __RESPONSE_CODES_HPP__

#include <cstdint>
#include "protocol.pb.h"

// =============================================================================
// Response Status Code Conversion Functions
// =============================================================================

/**
 * @brief Convert ResponseStatusCode to human-readable string
 * This provides a cleaner API for the STM32 code while using the protocol enum
 */
inline const char* response_status_code_to_string(ResponseStatusCode status) noexcept {
    switch (status) {
        case ResponseStatusCode_RESPONSE_STATUS_SUCCESS: return "Success";
        case ResponseStatusCode_RESPONSE_STATUS_FAILURE: return "Failure";
        case ResponseStatusCode_RESPONSE_STATUS_NOT_FOUND: return "Not found";
        case ResponseStatusCode_RESPONSE_STATUS_ALREADY_EXISTS: return "Already exists";
        case ResponseStatusCode_RESPONSE_STATUS_INVALID_STATE: return "Invalid state";
        case ResponseStatusCode_RESPONSE_STATUS_INVALID_PARAMETERS: return "Invalid parameters";
        case ResponseStatusCode_RESPONSE_STATUS_TIMEOUT: return "Timeout";
        case ResponseStatusCode_RESPONSE_STATUS_NO_MEMORY: return "No memory";
        case ResponseStatusCode_RESPONSE_STATUS_NO_PERMISSION: return "No permission";
        case ResponseStatusCode_RESPONSE_STATUS_UNSUPPORTED_COMMAND: return "Unsupported command";
        default: return "Unknown";
    }
}

#endif

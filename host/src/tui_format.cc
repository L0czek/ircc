#include "tui_format.hpp"
#include <cmath>
#include <format>

namespace tuifmt {

const char* charge_status_name(ChargeStatus s) {
    switch (s) {
        case ChargeStatus_CHARGE_STATUS_NOT_CHARGING:   return "Not charging";
        case ChargeStatus_CHARGE_STATUS_TRICKLE_CHARGE: return "Trickle";
        case ChargeStatus_CHARGE_STATUS_PRECHARGE:      return "Precharge";
        case ChargeStatus_CHARGE_STATUS_FAST_CHARGE:    return "Fast (CC)";
        case ChargeStatus_CHARGE_STATUS_TAPER_CHARGE:   return "Taper (CV)";
        case ChargeStatus_CHARGE_STATUS_TOP_OFF:        return "Top off";
        case ChargeStatus_CHARGE_STATUS_CHARGE_DONE:    return "Done";
        default:                                        return "Unknown";
    }
}

const char* input_source_name(ChargerInputSource s) {
    switch (s) {
        case ChargerInputSource_CHARGER_INPUT_USB_DPDM: return "USB D+/D-";
        case ChargerInputSource_CHARGER_INPUT_AC1:      return "VAC1 (USB-C)";
        case ChargerInputSource_CHARGER_INPUT_AC2:      return "VAC2 (dock)";
        case ChargerInputSource_CHARGER_INPUT_AUTO:     return "Auto";
        default:                                        return "?";
    }
}

const char* vbus_status_name(VBUSStatus s) {
    switch (s) {
        case VBUSStatus_VBUS_STATUS_NO_INPUT:              return "No input";
        case VBUSStatus_VBUS_STATUS_USB_SDP_500MA:         return "USB SDP";
        case VBUSStatus_VBUS_STATUS_USB_CDP_1500MA:        return "USB CDP";
        case VBUSStatus_VBUS_STATUS_USB_DCP_3250MA:        return "USB DCP";
        case VBUSStatus_VBUS_STATUS_ADJUSTABLE_HIGH:       return "HV adapter";
        case VBUSStatus_VBUS_STATUS_UNKNOWN_ADAPTER:       return "Unknown adapter";
        case VBUSStatus_VBUS_STATUS_NON_STANDARD_ADAPTER:  return "Non-std adapter";
        case VBUSStatus_VBUS_STATUS_OTG_MODE:              return "OTG";
        case VBUSStatus_VBUS_STATUS_NOT_QUALIFIED_ADAPTER: return "Not qualified";
        case VBUSStatus_VBUS_STATUS_POWERED_FROM_VBUS:     return "Powered from VBUS";
        default:                                           return "?";
    }
}

const char* regulator_mode_name(OutputRegulatorOperatingMode m) {
    switch (m) {
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK:            return "Buck";
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BOOST:           return "Boost";
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_BUCK_BOOST:      return "Buck-Boost";
        case OutputRegulatorOperatingMode_OUTPUT_REGULATOR_OPERATING_MODE_FREQUENCY_LIMIT: return "Freq limit";
        default:                                                                           return "?";
    }
}

const char* scope_state_name(ScopeState s) {
    switch (s) {
        case ScopeState_SCOPE_STATE_IDLE:        return "Idle";
        case ScopeState_SCOPE_STATE_PRETRIGGER:  return "Armed";
        case ScopeState_SCOPE_STATE_POSTTRIGGER: return "Triggered";
        case ScopeState_SCOPE_STATE_STOPPING:    return "Stopping";
        case ScopeState_SCOPE_STATE_DONE:        return "Done";
        case ScopeState_SCOPE_STATE_ERROR:       return "Error";
        default:                                 return "?";
    }
}

const char* trigger_kind_name(ScopeTriggerKind k) {
    switch (k) {
        case ScopeTriggerKind_SCOPE_TRIGGER_SOFTWARE:        return "software";
        case ScopeTriggerKind_SCOPE_TRIGGER_ANALOG_WATCHDOG: return "AWD";
        case ScopeTriggerKind_SCOPE_TRIGGER_COMPARATOR:      return "COMP";
        case ScopeTriggerKind_SCOPE_TRIGGER_EXTERNAL:        return "external";
        default:                                             return "?";
    }
}

namespace {

struct Flag {
    uint32_t bit;
    const char* name;
};

// BQ25792 ChargerStatus.status_flags (REG1B | REG1D << 8 | REG1E << 16 | REG1F << 24)
constexpr Flag kChargerRegulation[] = {
    {1u << 7, "IINDPM"}, {1u << 6, "VINDPM"}, {1u << 10, "TREG"}, {1u << 20, "VSYSMIN"},
};
constexpr Flag kChargerWarnings[] = {
    {1u << 5, "WD expired"}, {1u << 4, "poor source"}, {1u << 19, "CHG timer"}, {1u << 18, "trickle timer"},
    {1u << 17, "precharge timer"}, {1u << 24, "TS hot"}, {1u << 25, "TS warm"}, {1u << 26, "TS cool"},
    {1u << 27, "TS cold"}, {1u << 28, "VBAT low (OTG)"},
};
// ChargerStatus.fault_flags (REG20 | REG21 << 8)
constexpr Flag kChargerFaults[] = {
    {1u << 0, "VAC1 OVP"}, {1u << 1, "VAC2 OVP"}, {1u << 2, "CONV OCP"}, {1u << 3, "IBAT OCP"},
    {1u << 4, "IBUS OCP"}, {1u << 5, "VBAT OVP"}, {1u << 6, "VBUS OVP"},
    {1u << 10, "TSHUT"}, {1u << 12, "OTG UVP"}, {1u << 13, "OTG OVP"}, {1u << 14, "VSYS OVP"},
    {1u << 15, "VSYS SHORT"},
};

template <size_t N>
std::string flag_names(uint32_t value, const Flag (&table)[N]) {
    std::string out;
    for (const Flag& f : table) {
        if (value & f.bit) {
            out += (out.empty() ? "" : " ") + std::string(f.name);
        }
    }
    return out;
}

} // namespace

std::string reset_cause(uint32_t csr) {
    static constexpr Flag kReset[] = {
        {1u << 31, "low-power"}, {1u << 30, "WWDG"}, {1u << 29, "IWDG"}, {1u << 28, "software"},
        {1u << 27, "BOR"}, {1u << 26, "pin"}, {1u << 25, "option bytes"},
    };
    const std::string names = flag_names(csr, kReset);
    return names.empty() ? "none" : names;
}

const char* device_log_level_name(DeviceLogLevel level) {
    switch (level) {
        case DeviceLogLevel_DEVICE_LOG_TRACE: return "TRACE";
        case DeviceLogLevel_DEVICE_LOG_DEBUG: return "DEBUG";
        case DeviceLogLevel_DEVICE_LOG_INFO:  return "INFO";
        case DeviceLogLevel_DEVICE_LOG_WARN:  return "WARN";
        case DeviceLogLevel_DEVICE_LOG_ERROR: return "ERROR";
        default:                              return "?";
    }
}

std::optional<DeviceLogLevel> parse_device_log_level(const std::string& s) {
    if (s == "trace") return DeviceLogLevel_DEVICE_LOG_TRACE;
    if (s == "debug") return DeviceLogLevel_DEVICE_LOG_DEBUG;
    if (s == "info") return DeviceLogLevel_DEVICE_LOG_INFO;
    if (s == "warn" || s == "warning") return DeviceLogLevel_DEVICE_LOG_WARN;
    if (s == "error") return DeviceLogLevel_DEVICE_LOG_ERROR;
    return std::nullopt;
}

std::string charger_faults(uint32_t fault_flags) {
    return flag_names(fault_flags, kChargerFaults);
}

std::string charger_limits(uint32_t status_flags, uint32_t fault_flags) {
    std::string out = flag_names(status_flags, kChargerRegulation);
    if (fault_flags & (1u << 7)) {   // REG20 IBAT_REG: discharge current regulation, not a fault
        out += (out.empty() ? "" : " ") + std::string("IBAT REG");
    }
    return out;
}

std::string charger_warnings(uint32_t status_flags) {
    return flag_names(status_flags, kChargerWarnings);
}

ChargerInputs charger_inputs(uint32_t status_flags) {
    return {(status_flags & (1u << 0)) != 0, (status_flags & (1u << 1)) != 0, (status_flags & (1u << 2)) != 0,
            (status_flags & (1u << 3)) != 0};
}

const char* usb_pd_state_name(UsbPdState s) {
    switch (s) {
        case UsbPdState_USB_PD_STATE_DETACHED:   return "Detached";
        case UsbPdState_USB_PD_STATE_ATTACHED:   return "Attached (Type-C)";
        case UsbPdState_USB_PD_STATE_CONTRACT:   return "PD contract";
        case UsbPdState_USB_PD_STATE_HARD_RESET: return "Hard reset";
        default:                                 return "?";
    }
}

const char* usb_pd_power_role_name(UsbPdPowerRole r) {
    switch (r) {
        case UsbPdPowerRole_USB_PD_POWER_ROLE_NONE:   return "none";
        case UsbPdPowerRole_USB_PD_POWER_ROLE_SINK:   return "Sink";
        case UsbPdPowerRole_USB_PD_POWER_ROLE_SOURCE: return "Source";
        default:                                      return "?";
    }
}

const char* usb_pd_data_role_name(UsbPdDataRole r) {
    switch (r) {
        case UsbPdDataRole_USB_PD_DATA_ROLE_NONE: return "none";
        case UsbPdDataRole_USB_PD_DATA_ROLE_UFP:  return "UFP";
        case UsbPdDataRole_USB_PD_DATA_ROLE_DFP:  return "DFP";
        default:                                  return "?";
    }
}

const char* typec_current_name(TypeCCurrent c) {
    switch (c) {
        case TypeCCurrent_TYPEC_CURRENT_NONE:    return "none";
        case TypeCCurrent_TYPEC_CURRENT_DEFAULT: return "USB default";
        case TypeCCurrent_TYPEC_CURRENT_1A5:     return "1.5 A";
        case TypeCCurrent_TYPEC_CURRENT_3A0:     return "3.0 A";
        default:                                 return "?";
    }
}

const char* usb_pd_source_state_name(UsbPdSourceState s) {
    switch (s) {
        case UsbPdSourceState_USB_PD_SOURCE_DISABLED:        return "off";
        case UsbPdSourceState_USB_PD_SOURCE_READY:           return "ready";
        case UsbPdSourceState_USB_PD_SOURCE_ACTIVE:          return "supplying 5 V";
        case UsbPdSourceState_USB_PD_SOURCE_BLOCKED_DOCK:    return "blocked (dock)";
        case UsbPdSourceState_USB_PD_SOURCE_BLOCKED_BATTERY: return "blocked (battery)";
        case UsbPdSourceState_USB_PD_SOURCE_BLOCKED_VOUT:    return "blocked (VOUT not 5 V/3 A)";
        default:                                             return "?";
    }
}

const char* usb_pd_source_path_name(UsbPdSourcePath p) {
    switch (p) {
        case UsbPdSourcePath_USB_PD_SOURCE_PATH_OTG:  return "battery (OTG)";
        case UsbPdSourcePath_USB_PD_SOURCE_PATH_VOUT: return "VOUT";
        default:                                      return "-";
    }
}

// USB PD 3.x PDO layouts: type in [31:30]; APDO subtype in [29:28]
std::string describe_pdo(uint32_t raw) {
    auto bits = [raw](unsigned hi, unsigned lo) { return (raw >> lo) & ((1u << (hi - lo + 1)) - 1u); };
    switch (raw >> 30) {
        case 0:   // fixed: V [19:10] 50 mV, I [9:0] 10 mA
            return std::format("Fixed {:.2f} V {:.2f} A", bits(19, 10) * 0.05, bits(9, 0) * 0.01);
        case 1:   // battery: Vmax [29:20], Vmin [19:10] 50 mV, P [9:0] 250 mW
            return std::format("Battery {:.2f}-{:.2f} V {:.2f} W", bits(19, 10) * 0.05, bits(29, 20) * 0.05,
                               bits(9, 0) * 0.25);
        case 2:   // variable: Vmax [29:20], Vmin [19:10] 50 mV, I [9:0] 10 mA
            return std::format("Variable {:.2f}-{:.2f} V {:.2f} A", bits(19, 10) * 0.05, bits(29, 20) * 0.05,
                               bits(9, 0) * 0.01);
        default:
            if (bits(29, 28) == 0) {   // SPR PPS: Vmax [24:17], Vmin [15:8] 100 mV, I [6:0] 50 mA
                return std::format("PPS {:.2f}-{:.2f} V {:.2f} A", bits(15, 8) * 0.1, bits(24, 17) * 0.1,
                                   bits(6, 0) * 0.05);
            }
            return std::format("APDO type {} 0x{:08X}", bits(29, 28), raw);   // EPR / SPR AVS
    }
}

std::string usb_pd_summary(const UsbPdStatus& p) {
    const bool sink = p.power_role == UsbPdPowerRole_USB_PD_POWER_ROLE_SINK;
    switch (p.state) {
        case UsbPdState_USB_PD_STATE_DETACHED:
            return "detached";
        case UsbPdState_USB_PD_STATE_HARD_RESET:
            return std::string(usb_pd_power_role_name(p.power_role)) + " hard reset";
        case UsbPdState_USB_PD_STATE_ATTACHED:
            if (sink && p.has_typec_current) {
                return std::string("Sink Type-C Rp ") + typec_current_name(p.typec_current);
            }
            return std::string(usb_pd_power_role_name(p.power_role)) + " Type-C";
        case UsbPdState_USB_PD_STATE_CONTRACT: {
            std::string s = usb_pd_power_role_name(p.power_role);
            if (p.has_spec_revision) s += std::format(" PD{}", p.spec_revision);
            if (p.has_contract_voltage) s += std::format(" {:.2f} V", p.contract_voltage);
            if (p.has_contract_current) s += std::format(" {:.2f} A", p.contract_current);
            return s;
        }
        default:
            return "?";
    }
}

std::string format_time(double seconds) {
    const double a = std::fabs(seconds);
    if (a == 0.0) return "0";
    if (a < 1e-3) return std::format("{:.4g} us", seconds * 1e6);
    if (a < 1.0) return std::format("{:.4g} ms", seconds * 1e3);
    return std::format("{:.4g} s", seconds);
}

std::vector<std::string> split_words(const std::string& line) {
    std::vector<std::string> words;
    std::string cur;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') {
            quoted = !quoted;
        } else if (c == ' ' && !quoted) {
            if (!cur.empty()) words.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) words.push_back(std::move(cur));
    return words;
}

// "key=value" -> {key, value}; bare word -> {word, ""}
std::pair<std::string, std::string> key_value(const std::string& word) {
    const auto eq = word.find('=');
    if (eq == std::string::npos) return {word, ""};
    return {word.substr(0, eq), word.substr(eq + 1)};
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const auto pos = s.find(sep, start);
        parts.push_back(s.substr(start, pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return parts;
}
} // namespace tuifmt

#ifndef __TUI_FORMAT_HPP__
#define __TUI_FORMAT_HPP__

// Formatting and parsing helpers shared by the hostctl TUI modules.

#include "protocol.pb.h"
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace tuifmt {

const char* charge_status_name(ChargeStatus s);
const char* input_source_name(ChargerInputSource s);
const char* vbus_status_name(VBUSStatus s);
const char* regulator_mode_name(OutputRegulatorOperatingMode m);
const char* scope_state_name(ScopeState s);
const char* trigger_kind_name(ScopeTriggerKind k);
const char* device_log_level_name(DeviceLogLevel level);
std::optional<DeviceLogLevel> parse_device_log_level(const std::string& s);
std::string reset_cause(uint32_t csr);

// Decoded BQ25792 flag groups (ChargerStatus.status_flags / fault_flags), "" if none
std::string charger_faults(uint32_t fault_flags);
std::string charger_limits(uint32_t status_flags, uint32_t fault_flags);
std::string charger_warnings(uint32_t status_flags);

// ChargerStatus.status_flags bits 0..3 (REG1B): which inputs the BQ25792 sees
struct ChargerInputs {
    bool vbus;          // VBUS_PRESENT
    bool usb_c;         // AC1_PRESENT: VAC1 = USB-C
    bool dock;          // AC2_PRESENT: VAC2 = dock DC
    bool power_good;    // PG
};
ChargerInputs charger_inputs(uint32_t status_flags);

// USB-C / USB-PD (UsbPdStatus, docs/usb_pd.md)
const char* usb_pd_state_name(UsbPdState s);
const char* usb_pd_power_role_name(UsbPdPowerRole r);
const char* usb_pd_data_role_name(UsbPdDataRole r);
const char* typec_current_name(TypeCCurrent c);
const char* usb_pd_source_state_name(UsbPdSourceState s);
const char* usb_pd_source_path_name(UsbPdSourcePath p);
// Raw PDO -> "Fixed 9.00 V 3.00 A", "PPS 3.30-21.00 V 3.00 A", ...
std::string describe_pdo(uint32_t raw);
// One line: "Sink PD3 20.00 V 3.00 A", "Sink Type-C Rp 1.5 A", "detached", ...
std::string usb_pd_summary(const UsbPdStatus& p);

// "1.5 ms", "250 us", ... for axis labels
std::string format_time(double seconds);

// Command-line parsing
std::vector<std::string> split_words(const std::string& line);
std::pair<std::string, std::string> key_value(const std::string& word);
std::vector<std::string> split(const std::string& s, char sep);

template <typename T>
std::optional<T> parse_number(const std::string& s) {
    T value{};
    const char* first = s.data();
    const char* last = s.data() + s.size();
    if constexpr (std::is_integral_v<T>) {
        int base = 10;
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            first += 2;
            base = 16;
        }
        auto [ptr, ec] = std::from_chars(first, last, value, base);
        if (ec != std::errc() || ptr != last) return std::nullopt;
    } else {
        auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc() || ptr != last) return std::nullopt;
    }
    return value;
}

} // namespace tuifmt

#endif

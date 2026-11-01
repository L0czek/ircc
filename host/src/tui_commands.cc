// hostctl commands, forms, completion and device message handling (see tui.hpp)

#include "tui.hpp"
#include "error_host.hpp"
#include "tasks.hpp"
#include "tui_format.hpp"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <cstring>
#include <format>

using namespace tuifmt;

namespace {

constexpr const char *kTabNames[Tui::kTabCount] = {"overview", "power", "scope", "log", "device"};

bool fail(std::string &error, std::string message) {
    error = std::move(message);
    return false;
}

// "scope config" arguments -> ScopeConfigure
bool parse_scope_config(const std::vector<std::string> &args, ScopeConfigure &c, std::string &error) {
    c = ScopeConfigure_init_zero;
    bool have_rate = false, have_post = false;
    for (const auto &arg : args) {
        const auto [key, value] = key_value(arg);
        if (value.empty()) {
            continue;   // empty form fields
        }
        if (key == "ch" || (key.size() == 3 && key.starts_with("ch") && std::isdigit(key[2]))) {
            // ch=<adc>:<input>[:<sampling_time 0..7>]
            const auto parts = split(value, ':');
            if (c.channels_count >= 4 || parts.size() < 2 || parts.size() > 3) {
                return fail(error, "ch=<adc>:<input>[:<smp 0..7>], at most 4 channels");
            }
            auto &ch = c.channels[c.channels_count];
            const auto adc = parse_number<uint32_t>(parts[0]);
            const auto input = parse_number<uint32_t>(parts[1]);
            if (!adc || !input || *adc < 1 || *adc > 5) {
                return fail(error, "bad channel '" + value + "' (adc 1..5)");
            }
            ch.adc = *adc;
            ch.input = *input;
            if (parts.size() == 3) {
                const auto smp = parse_number<uint32_t>(parts[2]);
                if (!smp || *smp > 7) {
                    return fail(error, "sampling time index is 0..7");
                }
                ch.has_sampling_time = true;
                ch.sampling_time = *smp;
            }
            ++c.channels_count;
        } else if (key == "trig") {
            c.has_trigger = true;
            c.trigger.has_software = true;
            c.trigger.software = false;
            for (const auto &t : split(value, ',')) {
                if (t == "sw") {
                    c.trigger.software = true;
                } else if (t == "awd") {
                    c.trigger.has_analog_watchdog = true;
                    c.trigger.analog_watchdog = true;
                } else if (t == "ext") {
                    c.trigger.has_external = true;
                    c.trigger.external = true;
                } else if (t.starts_with("comp")) {
                    const auto n = parse_number<uint32_t>(t.substr(4));
                    if (!n || *n < 1 || *n > 7) {
                        return fail(error, "comp1..comp7");
                    }
                    c.trigger.has_comparator = true;
                    c.trigger.comparator = *n;
                } else {
                    return fail(error, "trig=sw,awd,comp<N>,ext");
                }
            }
        } else if (key == "awd") {
            // awd=<channel index>:<low>:<high>
            const auto parts = split(value, ':');
            std::optional<uint32_t> ch, lo, hi;
            if (parts.size() == 3) {
                ch = parse_number<uint32_t>(parts[0]);
                lo = parse_number<uint32_t>(parts[1]);
                hi = parse_number<uint32_t>(parts[2]);
            }
            if (!ch || !lo || !hi) {
                return fail(error, "awd=<ch>:<low>:<high>");
            }
            c.has_trigger = true;
            c.trigger.has_analog_watchdog = true;
            c.trigger.analog_watchdog = true;
            c.trigger.has_awd_channel = true;
            c.trigger.awd_channel = *ch;
            c.trigger.has_awd_low = true;
            c.trigger.awd_low = *lo;
            c.trigger.has_awd_high = true;
            c.trigger.awd_high = *hi;
        } else if (key == "fullpre") {
            c.has_trigger = true;
            c.trigger.has_require_full_pretrigger = true;
            c.trigger.require_full_pretrigger = value != "0";
        } else {
            const auto n = parse_number<uint32_t>(value);
            if (!n) {
                return fail(error, "bad value in '" + arg + "'");
            }
            if (key == "rate") {
                c.sample_rate_hz = *n;
                have_rate = true;
            } else if (key == "pre") {
                c.pre_trigger_samples = *n;
            } else if (key == "post") {
                c.post_trigger_samples = *n;
                have_post = true;
            } else {
                return fail(error, "unknown setting '" + key + "'");
            }
        }
    }
    if (c.channels_count == 0 || !have_rate || !have_post) {
        return fail(error, "need ch=<adc>:<input>[:<smp>], rate=<Hz> and post=<n>");
    }
    return true;
}

std::string format_value(float v) {
    std::string s = std::format("{:.3f}", v);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

} // namespace

// =============================================================================
// Dispatch
// =============================================================================

void Tui::execute_command(const std::string &command_line) noexcept {
    add_log("> " + command_line);
    const auto words = split_words(command_line);
    if (words.empty()) {
        return;
    }
    const std::string &cmd = words[0];
    const std::vector<std::string> args(words.begin() + 1, words.end());

    if (cmd == "help" || cmd == "h" || cmd == "?") {
        show_help(args.empty() ? "" : args[0]);
    } else if (cmd == "quit" || cmd == "exit" || cmd == "q") {
        exit();
    } else if (cmd == "tab" || cmd == "view") {
        for (int i = 0; i < kTabCount; ++i) {
            if (!args.empty() && args[0] == kTabNames[i]) {
                tab = static_cast<Tab>(i);
                if (tab.load() == Tab::Device) request_device_info();
                return;
            }
        }
        add_log("Usage: tab overview|power|scope|log|device (or F1..F5)");
    } else if (cmd == "msgs" || cmd == "messages") {
        // msgs <lines> | msgs large | msgs clear
        if (args.size() == 1 && args[0] == "clear") {
            std::lock_guard lock(data_mutex);
            messages.clear();
            message_scroll = 0;
        } else if (args.size() == 1 && (args[0] == "large" || args[0] == "small")) {
            messages_large = args[0] == "large";
        } else if (const auto n = args.size() == 1 ? parse_number<size_t>(args[0]) : std::nullopt; n && *n >= 3 && *n <= 200) {
            message_lines = *n;
            messages_large = false;
        } else {
            add_log("Usage: msgs <lines 3..200> | msgs large|small | msgs clear   (F6 toggles large)");
        }
    } else if (cmd == "ping") {
        Request req = Request_init_zero;
        req.which_command = Request_ping_tag;
        connector.send(req);
    } else if (cmd == "status") {
        request_status();
    } else if (cmd == "info") {
        request_device_info();
        tab = Tab::Device;
    } else if (cmd == "log") {
        handle_log(args);
    } else if (cmd == "poll") {
        handle_poll(args);
    } else if (cmd == "charger" || cmd == "chg") {
        handle_charger(args);
    } else if (cmd == "out" || cmd == "output") {
        handle_output(args);
    } else if (cmd == "scope") {
        handle_scope(args);
    } else if (cmd == "link") {
        handle_link(args);
    } else if (cmd == "edit") {
        open_form(args.empty() ? "" : args[0]);
    } else if (cmd == "list" || cmd == "start" || cmd == "stop") {
        handle_applet(cmd, args);
    } else {
        handle_applet("", words);   // "<applet> [args]"
    }
}

void Tui::handle_poll(const std::vector<std::string> &args) noexcept {
    if (args.size() != 1) {
        add_log("Usage: poll <seconds>|off");
        return;
    }
    if (args[0] == "off" || args[0] == "0") {
        stop_background_task(BackGroundTasks::StatusPoll);
        add_log("Status polling off (trend charts stop)");
        return;
    }
    const auto seconds = parse_number<double>(args[0]);
    if (!seconds || *seconds < 0.1) {
        add_log("poll: interval must be >= 0.1 s");
        return;
    }
    schedule_background_task(BackGroundTasks::StatusPoll,
                             std::chrono::duration_cast<PeriodicTask::duration_t>(std::chrono::duration<double>(*seconds)),
                             &status_poll_task);
    add_log(std::format("Polling status every {} s", *seconds));
}

void Tui::handle_log(const std::vector<std::string> &args) noexcept {
    if (!args.empty() && args[0] == "filter") {
        // log filter <level> [text...]  |  log filter off
        if (args.size() < 2 || args[1] == "off") {
            log_filter_level = DeviceLogLevel_DEVICE_LOG_TRACE;
            log_filter_text.clear();
            add_log("Device log filter cleared");
            return;
        }
        const auto level = parse_device_log_level(args[1]);
        if (!level) {
            add_log("Usage: log filter <trace|debug|info|warn|error> [text] | log filter off");
            return;
        }
        log_filter_level = *level;
        log_filter_text.clear();
        for (size_t i = 2; i < args.size(); ++i) {
            log_filter_text += (i > 2 ? " " : "") + args[i];
        }
        tab = Tab::Log;
        return;
    }
    if (!args.empty() && args[0] == "clear") {
        std::lock_guard lock(data_mutex);
        device_log.clear();
        return;
    }
    Request req = Request_init_zero;
    req.which_command = Request_log_subscribe_tag;
    auto &sub = req.command.log_subscribe;
    sub.has_enabled = true;
    sub.enabled = !(args.size() == 1 && args[0] == "off");
    sub.has_replay = true;
    sub.replay = false;
    if (sub.enabled) {
        const auto level = parse_device_log_level(args.empty() ? "info" : args[0]);
        if (!level || args.size() > 1) {
            add_log("Usage: log [trace|debug|info|warn|error] | log off | log filter ... | log clear");
            return;
        }
        sub.has_min_level = true;
        sub.min_level = *level;
    }
    connector.send(req);
    add_log(sub.enabled ? "Device log on" : "Device log off");
}

// =============================================================================
// Power
// =============================================================================

bool Tui::handle_charger(const std::vector<std::string> &args) noexcept {
    Request req = Request_init_zero;
    req.which_command = Request_set_charger_config_tag;
    auto &c = req.command.set_charger_config;

    if (args.size() == 1 && (args[0] == "on" || args[0] == "off")) {
        c.has_enabled = true;
        c.enabled = args[0] == "on";
        connector.send(req);
        return true;
    }
    if (args.size() == 1 && args[0] == "edit") {
        open_form("charger");
        return true;
    }
    if (args.empty() || args[0] != "set" || args.size() < 2) {
        add_log("Usage: charger on|off | charger edit | charger set [vchg=V] [ichg=A] [vin=V] [iin=A] [src=usb|ac1|ac2|auto]");
        return false;
    }
    for (size_t i = 1; i < args.size(); ++i) {
        const auto [key, value] = key_value(args[i]);
        if (value.empty()) {
            continue;
        }
        if (key == "src") {
            if (value == "usb") c.input_source = ChargerInputSource_CHARGER_INPUT_USB_DPDM;
            else if (value == "ac1") c.input_source = ChargerInputSource_CHARGER_INPUT_AC1;
            else if (value == "ac2") c.input_source = ChargerInputSource_CHARGER_INPUT_AC2;
            else if (value == "auto") c.input_source = ChargerInputSource_CHARGER_INPUT_AUTO;
            else { add_log("charger: src must be usb, ac1, ac2 or auto"); return false; }
            c.has_input_source = true;
            continue;
        }
        const auto v = parse_number<float>(value);
        if (!v) { add_log("charger: bad value in '" + args[i] + "'"); return false; }
        if (key == "vchg") { c.has_charge_voltage_limit = true; c.charge_voltage_limit = *v; }
        else if (key == "ichg") { c.has_charge_current_limit = true; c.charge_current_limit = *v; }
        else if (key == "vin") { c.has_input_voltage_limit = true; c.input_voltage_limit = *v; }
        else if (key == "iin") { c.has_input_current_limit = true; c.input_current_limit = *v; }
        else { add_log("charger: unknown setting '" + key + "'"); return false; }
    }
    connector.send(req);
    return true;
}

bool Tui::handle_output(const std::vector<std::string> &args) noexcept {
    Request req = Request_init_zero;
    req.which_command = Request_set_output_regulator_config_tag;
    auto &c = req.command.set_output_regulator_config;

    if (args.size() == 1 && (args[0] == "on" || args[0] == "off")) {
        c.has_enabled = true;
        c.enabled = args[0] == "on";
        connector.send(req);
        return true;
    }
    if (args.size() == 1 && args[0] == "edit") {
        open_form("out");
        return true;
    }
    if (args.empty() || args[0] != "set" || args.size() < 2) {
        add_log("Usage: out on|off | out edit | out set [v=V] [ilim=A] [rsense=ohm]");
        return false;
    }
    for (size_t i = 1; i < args.size(); ++i) {
        const auto [key, value] = key_value(args[i]);
        if (value.empty()) {
            continue;
        }
        const auto v = parse_number<float>(value);
        if (!v) { add_log("out: bad value in '" + args[i] + "'"); return false; }
        if (key == "v") { c.has_output_voltage = true; c.output_voltage = *v; }
        else if (key == "ilim") { c.has_output_current_limit = true; c.output_current_limit = *v; }
        else if (key == "rsense") { c.has_sense_resistor_ohms = true; c.sense_resistor_ohms = *v; }
        else { add_log("out: unknown setting '" + key + "'"); return false; }
    }
    connector.send(req);
    return true;
}

// =============================================================================
// Oscilloscope
// =============================================================================

bool Tui::handle_scope(const std::vector<std::string> &args) noexcept {
    const std::string sub = args.empty() ? "" : args[0];
    const std::vector<std::string> rest(args.begin() + (args.empty() ? 0 : 1), args.end());
    Request req = Request_init_zero;

    if (sub == "arm") {
        req.which_command = Request_scope_arm_tag;
    } else if (sub == "trigger" || sub == "trig") {
        req.which_command = Request_scope_force_trigger_tag;
    } else if (sub == "stop") {
        req.which_command = Request_scope_stop_tag;
    } else if (sub == "status") {
        req.which_command = Request_scope_get_status_tag;
    } else if (sub == "cancel") {
        scope_client.cancel();
        return true;
    } else if (sub == "edit") {
        open_form("scope");
        return true;
    } else if (sub == "load") {
        std::string error;
        if (rest.size() != 1 || !scope_client.load_csv(rest[0], error)) {
            add_log("scope load: " + (rest.size() != 1 ? std::string("Usage: scope load <file.csv>") : error));
            return false;
        }
        scope_view_offset = 0;
        scope_view_length = 0;
        tab = Tab::Scope;
        add_log("Scope: loaded " + rest[0]);
        return true;
    } else if (sub == "view") {
        for (const auto &arg : rest) {
            const auto [key, value] = key_value(arg);
            if (key == "reset") {
                scope_view_offset = 0;
                scope_view_length = 0;
            } else if (key == "y" && (value == "auto" || value == "full")) {
                scope_y_full = value == "full";
            } else {
                add_log("Usage: scope view [y=auto|full] [reset]   keys: PgUp/PgDn zoom, Left/Right pan, Home all, End trigger");
                return false;
            }
        }
        tab = Tab::Scope;
        return true;
    } else if (sub == "config") {
        std::string error;
        if (!parse_scope_config(rest, req.command.scope_configure, error)) {
            add_log("scope config: " + error);
            return false;
        }
        req.which_command = Request_scope_configure_tag;
        std::lock_guard lock(data_mutex);
        scope_config = req.command.scope_configure;
    } else if (sub == "read") {
        const auto state = connector.get_state();
        const bool done = state.has_scope_status && state.scope_status.state == ScopeState_SCOPE_STATE_DONE;
        uint32_t mask = done ? (1u << state.scope_status.num_channels) - 1u : 0;
        int64_t start = done ? state.scope_status.first_sample : 0;
        uint32_t count = done ? static_cast<uint32_t>(state.scope_status.post_trigger_samples - state.scope_status.first_sample) : 0;
        const uint32_t rate = done ? state.scope_status.sample_rate_hz : 0;
        DataLink link = connector.aux_open() ? DataLink_DATA_LINK_AUX : DataLink_DATA_LINK_ORIGIN;
        std::string file = std::format("scope_{}.csv", std::time(nullptr));
        for (const auto &arg : rest) {
            const auto [key, value] = key_value(arg);
            if (key == "file") { file = value; continue; }
            if (key == "link") {
                if (value == "aux") link = DataLink_DATA_LINK_AUX;
                else if (value == "origin" || value == "ctrl") link = DataLink_DATA_LINK_ORIGIN;
                else { add_log("scope read: link=origin|aux"); return false; }
                continue;
            }
            if (key == "start") {
                const auto v = parse_number<int64_t>(value);
                if (!v) { add_log("scope read: bad start"); return false; }
                start = *v;
                continue;
            }
            const auto v = parse_number<uint32_t>(value);
            if (!v) { add_log("scope read: bad value in '" + arg + "'"); return false; }
            if (key == "ch") mask = *v;
            else if (key == "count") count = *v;
            else { add_log("scope read: unknown setting '" + key + "'"); return false; }
        }
        if (mask == 0 || count == 0) {
            add_log("scope read: no finished capture known - run 'scope status' or give ch=<mask> start=<n> count=<n>");
            return false;
        }
        if (link == DataLink_DATA_LINK_AUX && !connector.aux_open()) {
            add_log("scope read: link=aux needs --aux-port");
            return false;
        }
        std::string error;
        if (!scope_client.read(mask, start, count, rate, link, file, error)) {
            add_log("scope read: " + error);
            return false;
        }
        scope_view_offset = 0;
        scope_view_length = 0;
        tab = Tab::Scope;
        add_log(std::format("Scope: reading {} samples x mask 0x{:x} from {} into {}", count, mask, start, file));
        return true;
    } else {
        add_log("Usage: scope config|edit|arm|trigger|stop|status|read|cancel|load|view (help scope)");
        return false;
    }
    connector.send(req);
    return true;
}

// =============================================================================
// Link, applets
// =============================================================================

void Tui::handle_link(const std::vector<std::string> &args) noexcept {
    if (args.size() == 2 && args[0] == "baud") {
        const auto baud = parse_number<uint32_t>(args[1]);
        if (!baud || *baud == 0) {
            add_log("link baud: bad rate");
            return;
        }
        Request req = Request_init_zero;
        req.which_command = Request_data_link_configure_tag;
        req.command.data_link_configure.baudrate = *baud;
        requested_aux_baud = *baud;   // host port follows when the device confirms
        connector.send(req);
        return;
    }
    if (args.size() == 1 && args[0] == "status") {
        const auto st = connector.get_state();
        add_log(std::format("Control link: {} frames, {} CRC errors, {} header errors, {} bytes dropped",
                            st.link_stats.frames, st.link_stats.crc_errors, st.link_stats.header_errors,
                            st.link_stats.dropped_bytes));
        add_log(std::format("Aux link: {} ({}), {} frames, {} CRC errors, {} bytes dropped",
                            connector.aux_open() ? "open" : "closed",
                            st.aux_baudrate ? std::format("device {} Bd", st.aux_baudrate) : std::string("device baud unknown"),
                            st.aux_stats.frames, st.aux_stats.crc_errors, st.aux_stats.dropped_bytes));
        return;
    }
    add_log("Usage: link baud <rate> | link status");
}

void Tui::handle_applet(const std::string &verb, const std::vector<std::string> &args) noexcept {
    Request req = Request_init_default;
    if (verb == "list") {
        req.which_command = Request_applet_list_tag;
        connector.send(req);
        return;
    }
    if (args.empty()) {
        add_log("Usage: " + (verb.empty() ? std::string("<applet>") : verb) + " <applet> [args...]");
        return;
    }
    auto copy = [](char *dst, size_t size, const std::string &src) {
        std::snprintf(dst, size, "%s", src.c_str());
    };
    if (verb == "stop") {
        req.which_command = Request_applet_stop_tag;
        copy(req.command.applet_stop.applet_name, sizeof(req.command.applet_stop.applet_name), args[0]);
    } else if (verb == "start") {
        req.which_command = Request_applet_start_tag;
        auto &a = req.command.applet_start;
        copy(a.applet_name, sizeof(a.applet_name), args[0]);
        for (size_t i = 1; i < args.size() && a.args_count < 16; ++i) {
            copy(a.args[a.args_count++], sizeof(a.args[0]), args[i]);
        }
    } else {
        req.which_command = Request_applet_command_tag;
        auto &a = req.command.applet_command;
        copy(a.applet_name, sizeof(a.applet_name), args[0]);
        for (size_t i = 1; i < args.size() && a.args_count < 16; ++i) {
            copy(a.args[a.args_count++], sizeof(a.args[0]), args[i]);
        }
    }
    connector.send(req);
}

// =============================================================================
// Forms
// =============================================================================

void Tui::open_form(const std::string &which) noexcept {
    const auto state = connector.get_state();
    const SystemStatus &ss = state.system_status;
    Form f;
    if (which == "charger") {
        f.title = "Charger (BQ25792)";
        f.command = "charger";
        const auto &c = ss.charger_config;
        const bool have = ss.has_charger_config;
        const char *src = "";
        if (ss.has_charger) {
            switch (ss.charger.input_source) {
                case ChargerInputSource_CHARGER_INPUT_USB_DPDM: src = "usb"; break;
                case ChargerInputSource_CHARGER_INPUT_AC1: src = "ac1"; break;
                case ChargerInputSource_CHARGER_INPUT_AC2: src = "ac2"; break;
                default: src = "auto"; break;
            }
        }
        f.fields = {
            {"vchg", "Charge voltage limit [V]", have ? format_value(c.charge_voltage_limit) : "", "3.0 - 18.8"},
            {"ichg", "Charge current limit [A]", have ? format_value(c.charge_current_limit) : "", "0.05 - 5.0"},
            {"vin", "Input voltage limit [V]", have ? format_value(c.input_voltage_limit) : "", "3.6 - 22.0 (VINDPM)"},
            {"iin", "Input current limit [A]", have ? format_value(c.input_current_limit) : "", "0.1 - 3.3 (IINDPM)"},
            {"src", "Input source", src, "usb | ac1 (USB-C) | ac2 (dock) | auto"},
            {"enable", "Charging", ss.has_charger ? (ss.charger.enabled ? "on" : "off") : "", "on | off"},
        };
    } else if (which == "out" || which == "output") {
        f.title = "Output regulator (TPS55289)";
        f.command = "out";
        const auto &c = ss.output_regulator_config;
        f.fields = {
            {"v", "Output voltage [V]", c.has_output_voltage ? format_value(c.output_voltage) : "", "range set by the feedback divider"},
            {"ilim", "Current limit [A]", c.has_output_current_limit ? format_value(c.output_current_limit) : "", "ilim x rsense <= 63.5 mV"},
            {"rsense", "Sense resistor [ohm]", c.has_sense_resistor_ohms ? format_value(c.sense_resistor_ohms) : "", "board value, e.g. 0.01"},
            {"enable", "Output", ss.has_output_regulator ? (ss.output_regulator.enabled ? "on" : "off") : "", "on | off"},
        };
    } else if (which == "scope") {
        f.title = "Oscilloscope capture";
        f.command = "scope config";
        std::optional<ScopeConfigure> sc;
        {
            std::lock_guard lock(data_mutex);
            sc = scope_config;
        }
        std::array<std::string, 4> ch{"1:1", "", "", ""};
        std::string rate = "1000000", pre = "1000", post = "9000", trig = "sw", awd, fullpre = "1";
        if (sc) {
            for (pb_size_t i = 0; i < 4; ++i) {
                ch[i] = i < sc->channels_count
                            ? std::format("{}:{}{}", sc->channels[i].adc, sc->channels[i].input,
                                          sc->channels[i].has_sampling_time ? std::format(":{}", sc->channels[i].sampling_time) : "")
                            : "";
            }
            rate = std::to_string(sc->sample_rate_hz);
            pre = std::to_string(sc->pre_trigger_samples);
            post = std::to_string(sc->post_trigger_samples);
            if (sc->has_trigger) {
                const auto &t = sc->trigger;
                trig.clear();
                auto add = [&trig](const std::string &s) { trig += (trig.empty() ? "" : ",") + s; };
                if (!t.has_software || t.software) add("sw");
                if (t.analog_watchdog) add("awd");
                if (t.comparator) add(std::format("comp{}", t.comparator));
                if (t.external) add("ext");
                if (t.has_awd_channel) awd = std::format("{}:{}:{}", t.awd_channel, t.awd_low, t.awd_high);
                if (t.has_require_full_pretrigger) fullpre = t.require_full_pretrigger ? "1" : "0";
            }
        }
        f.fields = {
            {"ch0", "Channel 0", ch[0], "<adc 1..5>:<input>[:<smp 0..7>]"},
            {"ch1", "Channel 1", ch[1], "empty = unused"},
            {"ch2", "Channel 2", ch[2], ""},
            {"ch3", "Channel 3", ch[3], ""},
            {"rate", "Sample rate [Hz]", rate, "per channel"},
            {"pre", "Pre-trigger samples", pre, ""},
            {"post", "Post-trigger samples", post, ""},
            {"trig", "Trigger sources", trig, "sw,awd,comp<N>,ext"},
            {"awd", "Watchdog window", awd, "<ch>:<low>:<high> ADC codes"},
            {"fullpre", "Full pre-trigger", fullpre, "1 = ignore triggers until the history is full"},
        };
    } else {
        add_log("Usage: edit charger|out|scope");
        return;
    }
    f.fields.push_back({"", "Enter apply   Esc cancel   Tab/Up/Down move   Del clear", "", ""});
    form = std::move(f);
}

void Tui::apply_form() noexcept {
    const Form f = *form;
    std::vector<std::string> args;
    std::optional<std::string> enable;
    for (const auto &field : f.fields) {
        if (field.key.empty() || field.value.empty()) {
            continue;
        }
        if (field.key == "enable") {
            enable = field.value;
        } else {
            args.push_back(field.key + "=" + field.value);
        }
    }
    bool ok = true;
    if (f.command == "charger" || f.command == "out") {
        const bool is_charger = f.command == "charger";
        if (!args.empty()) {
            args.insert(args.begin(), "set");
            ok = is_charger ? handle_charger(args) : handle_output(args);
        }
        if (ok && enable) {
            if (*enable != "on" && *enable != "off") {
                add_log(f.command + ": enable must be on or off");
                ok = false;
            } else {
                ok = is_charger ? handle_charger({*enable}) : handle_output({*enable});
            }
        }
    } else {
        args.insert(args.begin(), "config");
        ok = handle_scope(args);
    }
    if (ok) {
        add_log(f.title + ": applied");
        form.reset();
    }   // on error the form stays open; the message strip shows why
}

// =============================================================================
// Completion
// =============================================================================

std::vector<std::string> Tui::completions(const std::vector<std::string> &words, bool trailing_space) const {
    static const std::vector<std::string> kTop = {"help", "ping", "status", "poll", "info", "log", "charger", "out",
                                                  "scope", "link", "edit", "tab", "msgs", "list", "start", "stop", "quit"};
    static const std::unordered_map<std::string, std::vector<std::string>> kSecond = {
        {"charger", {"on", "off", "set", "edit"}},
        {"out", {"on", "off", "set", "edit"}},
        {"scope", {"config", "edit", "arm", "trigger", "stop", "status", "read", "cancel", "load", "view"}},
        {"link", {"baud", "status"}},
        {"log", {"trace", "debug", "info", "warn", "error", "off", "filter", "clear"}},
        {"poll", {"off", "1", "5"}},
        {"help", {"scope", "keys", "power", "log"}},
        {"msgs", {"large", "small", "clear", "10", "20"}},
        {"tab", {"overview", "power", "scope", "log", "device"}},
        {"edit", {"charger", "out", "scope"}},
    };
    static const std::unordered_map<std::string, std::vector<std::string>> kKeys = {
        {"charger set", {"vchg=", "ichg=", "vin=", "iin=", "src="}},
        {"out set", {"v=", "ilim=", "rsense="}},
        {"scope config", {"ch=", "rate=", "pre=", "post=", "trig=", "awd=", "fullpre="}},
        {"scope read", {"file=", "ch=", "start=", "count=", "link="}},
        {"scope view", {"y=auto", "y=full", "reset"}},
        {"log filter", {"trace", "debug", "info", "warn", "error", "off"}},
    };
    static const std::unordered_map<std::string, std::vector<std::string>> kValues = {
        {"src=", {"src=usb", "src=ac1", "src=ac2", "src=auto"}},
        {"link=", {"link=origin", "link=aux"}},
        {"trig=", {"trig=sw", "trig=awd", "trig=comp1", "trig=ext"}},
        {"fullpre=", {"fullpre=0", "fullpre=1"}},
    };

    const size_t level = trailing_space ? words.size() : (words.empty() ? 0 : words.size() - 1);
    const std::string partial = (trailing_space || words.empty()) ? "" : words.back();
    std::vector<std::string> pool;
    if (level == 0) {
        pool = kTop;
        std::lock_guard lock(data_mutex);
        pool.insert(pool.end(), applet_names.begin(), applet_names.end());
    } else if (level == 1) {
        if (auto it = kSecond.find(words[0]); it != kSecond.end()) {
            pool = it->second;
        } else if (words[0] == "start" || words[0] == "stop") {
            std::lock_guard lock(data_mutex);
            pool = applet_names;
        }
    } else {
        const std::string context = words[0] + " " + words[1];
        if (auto it = kKeys.find(context); it != kKeys.end()) {
            pool = it->second;
            // Value completion for "key=" prefixes
            const auto eq = partial.find('=');
            if (eq != std::string::npos) {
                if (auto v = kValues.find(partial.substr(0, eq + 1)); v != kValues.end()) {
                    pool = v->second;
                }
            } else if (context != "scope config") {   // ch= may repeat in scope config
                std::erase_if(pool, [&](const std::string &k) {
                    for (size_t i = 2; i < words.size(); ++i) {
                        if (k.ends_with("=") && words[i].starts_with(k) && !(i == words.size() - 1 && !trailing_space)) {
                            return true;
                        }
                    }
                    return false;
                });
            }
        }
    }
    std::vector<std::string> out;
    for (const auto &c : pool) {
        if (c.starts_with(partial)) {
            out.push_back(c);
        }
    }
    return out;
}

// =============================================================================
// Device messages (io thread)
// =============================================================================

void Tui::record_trends(const SystemStatus &status) noexcept {
    const auto now = TrendSeries::clock::now();
    const auto keep = std::chrono::seconds(kTrendWindowsS.back());
    std::lock_guard lock(data_mutex);
    if (status.has_charger_measurements) {
        const auto &m = status.charger_measurements;
        trends[kVbus].add(now, m.vbus_voltage, keep);
        trends[kIbus].add(now, m.ibus_current, keep);
        trends[kVbat].add(now, m.vbat_voltage, keep);
        trends[kVsys].add(now, m.vsys_voltage, keep);
        if (m.has_ibat_current) trends[kIbat].add(now, m.ibat_current, keep);
        if (m.has_vac1_voltage) trends[kVac1].add(now, m.vac1_voltage, keep);
        if (m.has_vac2_voltage) trends[kVac2].add(now, m.vac2_voltage, keep);
    }
}

void Tui::on_message(const Response &resp, frame::Type type, uint16_t seq) noexcept {
    scope_client.on_message(resp, type, seq);

    switch (resp.which_payload) {
        case Response_system_status_tag:
            record_trends(resp.payload.system_status);
            break;
        case Response_scope_status_tag: {
            const auto &st = resp.payload.scope_status;
            if (type == frame::Type::Response) {
                std::string msg = std::string("Scope: ") + scope_state_name(st.state);
                if (st.state == ScopeState_SCOPE_STATE_DONE) {
                    msg += std::format(", {} ch @ {} Hz, samples {}..{}", st.num_channels, st.sample_rate_hz,
                                       st.first_sample, st.post_trigger_samples);
                }
                add_log(msg);
            }
            break;
        }
        case Response_scope_read_started_tag: {
            const auto &r = resp.payload.scope_read_started;
            add_log(std::format("Scope: transfer {} started, {} frames on {} link", r.transfer_id, r.frames,
                                r.link == DataLink_DATA_LINK_AUX ? "aux" : "control"));
            break;
        }
        case Response_data_link_status_tag: {
            const uint32_t actual = resp.payload.data_link_status.baudrate;
            const uint32_t requested = requested_aux_baud.exchange(0);
            std::string msg = std::format("Data link: device aux UART at {} Bd", actual);
            if (requested && connector.aux_open()) {
                msg += connector.set_aux_baudrate(requested) ? std::format(", host aux port set to {} Bd", requested)
                                                             : ", host aux port baud change FAILED";
            }
            add_log(msg);
            break;
        }
        case Response_log_message_tag: {
            const auto &m = resp.payload.log_message;
            {
                std::lock_guard lock(data_mutex);
                if (m.has_dropped && m.dropped) {
                    device_log.push_back({m.tick_ms, DeviceLogLevel_DEVICE_LOG_WARN,
                                          std::format("({} messages dropped)", m.dropped)});
                }
                device_log.push_back({m.tick_ms, m.level, m.text});
                while (device_log.size() > kMaxDeviceLog) {
                    device_log.pop_front();
                }
            }
            if (m.level >= DeviceLogLevel_DEVICE_LOG_WARN) {   // also in the message strip
                add_log(std::format("dev {}: {}", device_log_level_name(m.level), m.text));
            } else {
                refresh();
            }
            break;
        }
        case Response_device_info_tag: {
            std::lock_guard lock(data_mutex);
            device_info = resp.payload.device_info;
            break;
        }
        case Response_applet_list_response_tag: {
            std::lock_guard lock(data_mutex);
            applet_names.clear();
            const auto &list = resp.payload.applet_list_response;
            for (pb_size_t i = 0; i < list.applet_names_count; ++i) {
                applet_names.emplace_back(list.applet_names[i]);
            }
            break;
        }
        default:
            break;
    }
}

// =============================================================================
// Help
// =============================================================================

void Tui::show_help(const std::string &topic) noexcept {
    if (topic == "scope") {
        add_log("Oscilloscope (F3 shows the last capture):");
        add_log("  scope config ch=<adc>:<input>[:<smp>] [ch=...] rate=<Hz> pre=<n> post=<n>");
        add_log("               [trig=sw,awd,comp<N>,ext] [awd=<ch>:<low>:<high>] [fullpre=0|1]");
        add_log("  scope edit                  form with the current configuration");
        add_log("  scope arm | trigger | stop | status");
        add_log("  scope read [file=<csv>] [ch=<mask>] [start=<n>] [count=<n>] [link=origin|aux]");
        add_log("  scope cancel | scope load <csv> | scope view [y=auto|full] [reset]");
        add_log("  keys in F3: PgUp/PgDn zoom, Left/Right pan, Home whole capture, End centre trigger");
        return;
    }
    if (topic == "keys") {
        add_log("Keys: F1 overview  F2 power trends  F3 scope  F4 device log  F5 device info");
        add_log("  Tab complete   Up/Down history   Esc clear line   Ctrl-C / 'quit' exit");
        add_log("  F2: PgUp/PgDn trend window   F3: PgUp/PgDn zoom, Left/Right pan, Home, End");
        add_log("  F1/F4: PgUp/PgDn scroll the device log, End newest");
        add_log("  Messages: Ctrl-Up/Down or mouse wheel scroll, Esc (empty line) newest, F6 half screen");
        add_log("  Mouse wheel over a view: scroll the log (F1/F4), zoom (F3), trend window (F2)");
        return;
    }
    if (topic == "log") {
        add_log("Device log: log [trace|debug|info|warn|error] | log off | log clear");
        add_log("  log filter <level> [text]   show only >= level containing text (F4); log filter off");
        return;
    }
    if (topic == "power") {
        add_log("charger on|off | charger edit | charger set [vchg=V] [ichg=A] [vin=V] [iin=A] [src=usb|ac1|ac2|auto]");
        add_log("out on|off | out edit | out set [v=V] [ilim=A] [rsense=ohm]");
        add_log("poll <s>|off  (trend charts in F2 need polling)");
        return;
    }
    add_log("Commands (Tab completes; 'help scope|power|log|keys' for details):");
    add_log("  ping | status | info | poll <s>|off | log ... | tab <view> | msgs <n>|large|clear | quit");
    add_log("  charger on|off|set|edit   out on|off|set|edit   edit charger|out|scope");
    add_log("  scope config|edit|arm|trigger|stop|status|read|cancel|load|view");
    add_log("  link baud <rate> | link status");
    add_log("  list | start <applet> [args] | stop <applet> | <applet> [args]");
}

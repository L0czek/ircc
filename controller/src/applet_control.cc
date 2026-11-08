#include "applet_control.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

const char* applet_error_text(AppletError e) noexcept {
    switch (e) {
        case AppletError::OutOfRange: return "out of range";
        case AppletError::InvalidState: return "not now";
        case AppletError::Hardware: return "hardware error";
        case AppletError::Unsupported: return "unsupported";
        case AppletError::NotFound: return "not found";
        case AppletError::Busy: return "other applet running";
    }
    return "error";
}

const char* applet_run_text(AppletRun r) noexcept {
    switch (r) {
        case AppletRun::Stopped: return "STOP";
        case AppletRun::Running: return "RUN";
        case AppletRun::Fault: return "FAULT";
    }
    return "?";
}

int AppletControl::find_param(const char* key) const noexcept {
    const auto list = params();
    for (size_t i = 0; i < list.size(); ++i) {
        if (std::strcmp(list[i].key, key) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::expected<float, AppletError> check_param(const ParamDesc& desc, float value) noexcept {
    if (std::isnan(value)) {
        return std::unexpected(AppletError::OutOfRange);
    }
    switch (desc.type) {
        case ParamType::Bool:
            return value != 0.0f ? 1.0f : 0.0f;
        case ParamType::Choice: {
            const float index = std::round(value);
            if (index < 0.0f || index >= static_cast<float>(desc.choices.size())) {
                return std::unexpected(AppletError::OutOfRange);
            }
            return index;
        }
        case ParamType::Number:
            break;
    }
    // Tolerate rounding noise at the limits
    const float tolerance = desc.step > 0.0f ? desc.step * 0.01f : 0.0f;
    if (value < desc.min - tolerance || value > desc.max + tolerance) {
        return std::unexpected(AppletError::OutOfRange);
    }
    if (desc.step > 0.0f) {
        value = std::round(value / desc.step) * desc.step;
    }
    if (value < desc.min) value = desc.min;
    if (value > desc.max) value = desc.max;
    return value;
}

// ---------------------------------------------------------------------------------------
// AppletParamCommand: parsing only
// ---------------------------------------------------------------------------------------

namespace {

std::expected<float, AppletParamError> parse_param_value(const ParamDesc& desc, const char* text) noexcept {
    switch (desc.type) {
        case ParamType::Bool:
            return (std::strcmp(text, "on") == 0 || std::strcmp(text, "1") == 0 || std::strcmp(text, "true") == 0)
                       ? 1.0f
                       : 0.0f;
        case ParamType::Choice:
            for (size_t c = 0; c < desc.choices.size(); ++c) {
                if (std::strcmp(text, desc.choices[c]) == 0) {
                    return static_cast<float>(c);
                }
            }
            return std::unexpected(AppletParamError{AppletParamError::Kind::BadValue, text});
        case ParamType::Number:
            break;
    }
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text) {
        return std::unexpected(AppletParamError{AppletParamError::Kind::BadValue, text});
    }
    return value;
}

} // namespace

std::expected<AppletParamCommand, AppletParamError> AppletParamCommand::parse(const AppletControl& applet, size_t argc,
                                                                              const char* const* args) noexcept {
    using Kind = AppletParamError::Kind;
    if (argc == 0) {
        return std::unexpected(AppletParamError{Kind::NotParamCommand});
    }
    AppletParamCommand cmd;
    const char* verb = args[0];
    if (std::strcmp(verb, "params") == 0) {
        cmd.verb = Verb::Params;
        return cmd;
    }
    if (std::strcmp(verb, "get") == 0) {
        cmd.verb = Verb::Get;
        if (argc > 1) {
            cmd.param = applet.find_param(args[1]);
            if (cmd.param < 0) {
                return std::unexpected(AppletParamError{Kind::UnknownParam, args[1]});
            }
        }
        return cmd;
    }
    if (std::strcmp(verb, "set") == 0) {
        cmd.verb = Verb::Set;
        if (argc < 3) {
            return std::unexpected(AppletParamError{Kind::Usage});
        }
        cmd.param = applet.find_param(args[1]);
        if (cmd.param < 0) {
            return std::unexpected(AppletParamError{Kind::UnknownParam, args[1]});
        }
        const auto value = parse_param_value(applet.params()[static_cast<size_t>(cmd.param)], args[2]);
        if (!value) {
            return std::unexpected(value.error());
        }
        cmd.value = *value;
        return cmd;
    }
    return std::unexpected(AppletParamError{Kind::NotParamCommand});
}

// ---------------------------------------------------------------------------------------
// AppletParamText: reply text only
// ---------------------------------------------------------------------------------------

AppletParamText::AppletParamText(char* out, size_t capacity) noexcept : out_(out), capacity_(capacity) {
    if (capacity_ > 0) {
        out_[0] = '\0';
    }
}

void AppletParamText::write_reply(const AppletControl& applet, const AppletParamCommand& cmd) noexcept {
    switch (cmd.verb) {
        case AppletParamCommand::Verb::Params:
            write_param_list(applet);
            break;
        case AppletParamCommand::Verb::Get:
            for (size_t i = 0; i < applet.params().size(); ++i) {
                if (cmd.param < 0 || static_cast<size_t>(cmd.param) == i) {
                    write_value_line(applet, i);
                    append("\n");
                }
            }
            break;
        case AppletParamCommand::Verb::Set:
            write_value_line(applet, static_cast<size_t>(cmd.param));   // the value as applied
            break;
    }
}

void AppletParamText::write_parse_error(const AppletParamError& error) noexcept {
    switch (error.kind) {
        case AppletParamError::Kind::Usage: append("usage: set <key> <value>"); break;
        case AppletParamError::Kind::UnknownParam: append("unknown parameter %s", error.arg); break;
        case AppletParamError::Kind::BadValue: append("bad value %s", error.arg); break;
        case AppletParamError::Kind::NotParamCommand: append("not a parameter command"); break;
    }
}

void AppletParamText::write_set_error(const AppletControl& applet, const AppletParamCommand& cmd,
                                      AppletError error) noexcept {
    append("%s: %s", applet.params()[static_cast<size_t>(cmd.param)].key, applet_error_text(error));
}

void AppletParamText::write_param_list(const AppletControl& applet) noexcept {
    const auto params = applet.params();
    if (params.empty()) {
        append("no parameters");
        return;
    }
    for (const auto& d : params) {
        const char* live = (d.flags & kParamLive) ? " live" : "";
        switch (d.type) {
            case ParamType::Number:
                append("%s %.*f..%.*f%s%s\n", d.key, static_cast<int>(d.decimals), static_cast<double>(d.min),
                       static_cast<int>(d.decimals), static_cast<double>(d.max), d.unit, live);
                break;
            case ParamType::Bool:
                append("%s on|off%s\n", d.key, live);
                break;
            case ParamType::Choice:
                append("%s", d.key);
                for (size_t c = 0; c < d.choices.size(); ++c) {
                    append("%s%s", c ? "|" : " ", d.choices[c]);
                }
                append("\n");
                break;
        }
    }
}

void AppletParamText::write_value_line(const AppletControl& applet, size_t param) noexcept {
    const ParamDesc& desc = applet.params()[param];
    append("%s=", desc.key);
    write_value(desc, applet.get_param(param));
}

void AppletParamText::write_value(const ParamDesc& desc, float value) noexcept {
    switch (desc.type) {
        case ParamType::Choice: {
            const size_t c = static_cast<size_t>(value);
            append("%s", c < desc.choices.size() ? desc.choices[c] : "?");
            break;
        }
        case ParamType::Bool:
            append("%s", value != 0.0f ? "on" : "off");
            break;
        case ParamType::Number:
            append("%.*f%s", static_cast<int>(desc.decimals), static_cast<double>(value), desc.unit);
            break;
    }
}

void AppletParamText::append(const char* fmt, ...) noexcept {
    if (length_ >= capacity_) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(out_ + length_, capacity_ - length_, fmt, ap);
    va_end(ap);
    length_ += n > 0 ? static_cast<size_t>(n) : 0;
}

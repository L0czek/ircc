#ifndef __APPLET_CONTROL_HPP__
#define __APPLET_CONTROL_HPP__

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

/**
 * @brief The declarative side of an applet: parameters, readouts, run state, start/stop.
 *
 * One declaration drives every front end: the front panel builds its forms from it, and
 * the host CLI uses the generic `applet_command <name> params|get|set` commands. No HAL
 * types here so the panel UI also builds on the host (simulator, tests).
 *
 * Values are floats in the units shown to the user (the applet picks the unit, e.g.
 * "kHz" or "Hz"). Bool parameters are 0/1, choices are the index into `choices`.
 */

enum class ParamType : uint8_t {
    Number,
    Bool,
    Choice,
};

enum ParamFlags : uint8_t {
    kParamLive = 0x01,        // may change while running (applied as the encoder turns)
    kParamPersist = 0x02,     // part of presets / saved configuration
    kParamReadOnly = 0x04,    // shown, not editable
};

struct ParamDesc {
    const char* key;          // CLI name, no spaces: "freq"
    const char* label;        // panel label (<= 11 chars): "Frequency"
    const char* unit;         // "Hz", "%", "" ...
    ParamType type;
    float min;
    float max;
    float step;               // finest step (encoder x1)
    uint8_t decimals;
    std::span<const char* const> choices;   // ParamType::Choice
    uint8_t flags;
};

struct ReadoutDesc {
    const char* label;
    const char* unit;
    uint8_t decimals;
};

enum class AppletRun : uint8_t {
    Stopped,
    Running,
    Fault,
};

enum class AppletError : uint8_t {
    OutOfRange,
    InvalidState,      // e.g. not editable while running
    Hardware,
    Unsupported,
    NotFound,
    Busy,              // another applet is running
};

const char* applet_error_text(AppletError e) noexcept;
const char* applet_run_text(AppletRun r) noexcept;

using AppletResult = std::expected<void, AppletError>;

class AppletControl {
public:
    virtual const char* name() const noexcept = 0;
    /** @brief One line shown on the panel under the name */
    virtual const char* summary() const noexcept { return ""; }

    virtual std::span<const ParamDesc> params() const noexcept { return {}; }
    virtual float get_param(size_t index) const noexcept { (void)index; return 0.0f; }
    /** @brief Range-checked by the caller (check_param); applies at once if running and live */
    virtual AppletResult set_param(size_t index, float value) noexcept {
        (void)index;
        (void)value;
        return std::unexpected(AppletError::Unsupported);
    }

    virtual std::span<const ReadoutDesc> readouts() const noexcept { return {}; }
    virtual float readout(size_t index) const noexcept { (void)index; return 0.0f; }

    virtual AppletRun run_state() const noexcept = 0;
    virtual const char* fault_text() const noexcept { return nullptr; }

    /** @brief Start / stop. Use AppletRunner (one applet at a time), not these directly. */
    virtual AppletResult start() noexcept = 0;
    virtual void stop() noexcept = 0;

    /** @brief Drives a power stage: the panel asks for a hold-to-start */
    virtual bool needs_arming() const noexcept { return true; }

    /** @brief Index of a parameter by key, or -1 */
    int find_param(const char* key) const noexcept;

protected:
    ~AppletControl() = default;
};

/**
 * @brief Validate and quantise a value for a parameter: range check, step grid (Number),
 *        0/1 (Bool), index (Choice). Returns the value to apply.
 */
std::expected<float, AppletError> check_param(const ParamDesc& desc, float value) noexcept;

// ---------------------------------------------------------------------------------------
// Generic parameter commands: `applet_command <name> params | get [key] | set <key> <value>`
//
// Three separate steps, so that only the caller ever acts on an applet:
//   1. AppletParamCommand::parse()  text -> command                 (no side effects)
//   2. the caller applies a Set      AppletRunner::set_param()     (the only action)
//   3. AppletParamText               command / result -> reply text (no side effects)
// Shared by the firmware (MessageHandler) and the host emulator.
// ---------------------------------------------------------------------------------------

/** @brief Why a parameter command could not be parsed */
struct AppletParamError {
    enum class Kind : uint8_t {
        NotParamCommand,   // the verb is none of params / get / set: the applet's own command
        Usage,             // set without key and value
        UnknownParam,      // no parameter with that key
        BadValue,          // value text does not fit the parameter type
    };
    Kind kind;
    const char* arg = "";   // the offending key or value text
};

/** @brief A parsed parameter command (no side effects) */
struct AppletParamCommand {
    enum class Verb : uint8_t { Params, Get, Set };
    Verb verb = Verb::Params;
    int param = -1;        // Get: one parameter (-1 = all); Set: the parameter to change
    float value = 0.0f;    // Set: the parsed value (range checked by the setter)

    /** @brief args[0] is the verb, then its arguments */
    static std::expected<AppletParamCommand, AppletParamError> parse(const AppletControl& applet, size_t argc,
                                                                     const char* const* args) noexcept;
};

/**
 * @brief Reply text for the parameter commands, written into a fixed buffer (truncated).
 *        Formatting only: it reads the applet (declarations, current values), never changes it.
 */
class AppletParamText {
public:
    AppletParamText(char* out, size_t capacity) noexcept;

    /** @brief The reply to a command that succeeded (params / get list, set: the new value) */
    void write_reply(const AppletControl& applet, const AppletParamCommand& cmd) noexcept;
    /** @brief "unknown parameter freqq", "bad value abc", "usage: ..." */
    void write_parse_error(const AppletParamError& error) noexcept;
    /** @brief "freq: out of range" */
    void write_set_error(const AppletControl& applet, const AppletParamCommand& cmd, AppletError error) noexcept;

private:
    void write_param_list(const AppletControl& applet) noexcept;           // "freq 1..1000000Hz live\n"
    void write_value_line(const AppletControl& applet, size_t param) noexcept;   // "freq=1000Hz"
    void write_value(const ParamDesc& desc, float value) noexcept;         // "1000Hz", "on", "Burst"
    __attribute__((format(printf, 2, 3))) void append(const char* fmt, ...) noexcept;

    char* out_;
    size_t capacity_;
    size_t length_ = 0;
};

#endif

#ifndef __APPLER_HPP__
#define __APPLER_HPP__

#include "applet_control.hpp"
#include "config.hpp"
#include "error.hpp"
#include <array>
#include <cstdint>
#include <expected>
#include <cstring>

/**
 * @brief Applet: the declarative interface (AppletControl: parameters, readouts, run
 *        state, start/stop, used by the front panel and the generic CLI commands) plus the
 *        CLI-style configure/on_message and saved configuration.
 *
 * Start and stop go through AppletRunner (one applet at a time, shared by the host and the
 * panel). shutdown() is the legacy name of stop().
 */
class Applet : public AppletControl {
public:
    virtual std::expected<void, ControllerError> configure(int, char **) noexcept =0;
    virtual const char * help() const noexcept =0;
    virtual void shutdown() noexcept { stop(); }

    virtual uint32_t config_size() const noexcept =0;
    virtual void save_config(uint8_t data[]) const noexcept =0;
    virtual void load_config(const uint8_t data[]) noexcept =0;

    virtual std::expected<void, ControllerError> step() noexcept =0;
    virtual std::expected<void, ControllerError> on_message(int, char **) noexcept =0;
};

class AppletRegistry {
    mutable std::array<Applet*, APPLETS_N> applets;
    mutable uint32_t n;

    AppletRegistry() = default;
    AppletRegistry(const AppletRegistry &) = delete;
public:
    Applet* fetch_by_name(const char *) const noexcept;
    Applet* fetch_by_idx(const uint32_t ) const noexcept;
    void register_applet(Applet *) const noexcept;

    static AppletRegistry& load() noexcept;
    
    // Public access to applets array for listing
    const std::array<Applet*, APPLETS_N>& get_applets() const noexcept { return applets; }
    uint32_t get_count() const noexcept { return n; }
};

template <typename T>
struct AppletRegistrationEntry {
    AppletRegistrationEntry() {
        static T applet;

        AppletRegistry::load()
            .register_applet(&applet);
    }
};

// =============================================================================
// AppletRegistry Implementation
// =============================================================================

inline Applet* AppletRegistry::fetch_by_name(const char* name) const noexcept {
    if (!name) return nullptr;

    for (uint32_t i = 0; i < n; ++i) {
        if (applets[i] && std::strcmp(applets[i]->name(), name) == 0) {
            return applets[i];
        }
    }
    return nullptr;
}

inline Applet* AppletRegistry::fetch_by_idx(const uint32_t idx) const noexcept {
    if (idx >= n) return nullptr;
    return applets[idx];
}

inline void AppletRegistry::register_applet(Applet* applet) const noexcept {
    if (!applet || n >= APPLETS_N) return;
    applets[n++] = applet;
}

inline AppletRegistry& AppletRegistry::load() noexcept {
    static AppletRegistry registry;
    return registry;
}

#endif

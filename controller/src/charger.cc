#include "charger.hpp"
#include "i2c_bus.hpp"
#include "log.hpp"
#include "protocol.pb.h"
#include "cmsis_os2.h"
#include "board.h"
#include <cmath>
#include <algorithm>

namespace {

// Limits enforced by the BQ25792 driver; checked up front so a request is
// either applied completely or not at all.
bool in_range(float v, float lo, float hi) { return v >= lo && v <= hi; }  // false for NaN

bool valid_input_source(ChargerInputSource s) {
    return s == ChargerInputSource_CHARGER_INPUT_AC1 ||
           s == ChargerInputSource_CHARGER_INPUT_AC2 ||
           s == ChargerInputSource_CHARGER_INPUT_AUTO;
}

bool is_forced_input(ChargerInputSource s) {
    return s == ChargerInputSource_CHARGER_INPUT_AC1 || s == ChargerInputSource_CHARGER_INPUT_AC2;
}

// Register ranges (SLUSDG1D Tables 8-17, 8-18, 8-22, 8-23)
constexpr float kIindpmMinA = 0.1f;
constexpr float kIindpmMaxA = 3.3f;
constexpr float kVindpmMinV = 3.6f;
constexpr float kVindpmMaxV = 22.0f;
constexpr float kVotgMinV = 2.8f;
constexpr float kVotgMaxV = 22.0f;
constexpr float kIotgMinA = 0.12f;
constexpr float kIotgMaxA = 3.32f;

// Clamp to [lo, hi] (NaN -> lo) and round to the register step, in milli-units, so
// targets are compared at register resolution and float noise causes no writes.
uint32_t to_milli_steps(float value, float lo, float hi, uint32_t step_milli) {
    float v = value;
    if (!(v >= lo)) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return static_cast<uint32_t>(std::lround(v * 1000.0f / static_cast<float>(step_milli))) * step_milli;
}

float milli_to_unit(uint32_t milli) { return static_cast<float>(milli) / 1000.0f; }

// Does the mux read back select this input (the chip rewrites EN_ACDRV1/2 when it swaps)?
bool mux_selects(ChargerInputSource input, const bq25792::MuxState& m) {
    if (m.dis_acdrv) {
        return false;
    }
    if (input == ChargerInputSource_CHARGER_INPUT_AC1) {
        return m.en_acdrv1 && !m.en_acdrv2;
    }
    return m.en_acdrv2 && !m.en_acdrv1;
}

} // namespace

Charger::Charger(I2C_HandleTypeDef* hi2c)
    : driver_(hi2c),
      update_sem_(1, 0, nullptr),
      last_update_ms_(0)
{
    (void)i2c_internal_bus_mutex();  // create the bus mutex before any thread uses it
}

Charger::~Charger() = default;

std::unexpected<ControllerError> Charger::fail(const char* what, const std::error_code& ec) const noexcept {
    const ControllerError mapped = from_bq25792_error(ec);
    if (log_limiter_.allow(what, mapped, os::timestamp_ms())) {
        os::error("Charger: %s failed: %s [%s] (%lu repeats suppressed)\n", what, to_string(mapped),
                  get_error_source(ec), static_cast<unsigned long>(log_limiter_.suppressed()));
    }
    return std::unexpected(mapped);
}

// =============================================================================
// Lifecycle / housekeeping
// =============================================================================

std::expected<void, ControllerError> Charger::initialize() noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        auto r = check("initialize", driver_.initialize());
        if (!r) {
            return r;
        }
        // Also covers a chip that kept running across an MCU reset (still in host mode).
        return reapply_all_locked();
    });
}

std::expected<void, ControllerError> Charger::service() noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        if (!driver_.is_initialized()) {
            // Charger was absent / failed at boot: keep retrying so it recovers.
            auto init = check("initialize", driver_.initialize());
            if (!init) {
                return init;
            }
            auto reapply = reapply_all_locked();
            if (!reapply) {
                return reapply;
            }
        } else {
            // Check before kicking: any write restarts an expired watchdog.
            auto expired = check("read watchdog status", driver_.is_watchdog_expired());
            if (!expired) {
                return std::unexpected(expired.error());
            }
            if (!*expired) {
                auto r = check("kick watchdog", driver_.kick_watchdog());
                if (!r) {
                    return r;
                }
            }
            auto r = handle_watchdog_locked(*expired);
            if (!r) {
                return r;
            }
        }

        // Retry what failed part-way earlier.
        if (settings_dirty_) {
            auto r = apply_settings_locked(settings_);
            if (!r) {
                return r;   // stays dirty, retried on the next service() call
            }
            settings_dirty_ = false;
        }
        if (has_target_ && target_dirty_) {
            return apply_power_path_locked();
        }
        return {};
    });
}

std::expected<void, ControllerError> Charger::handle_watchdog_locked(bool wd_expired) noexcept {
    if (!driver_.is_initialized()) {
        return std::unexpected(ControllerError::BQ25792_NotInitialized);  // service() retries the init
    }
    if (wd_expired) {
        // Default mode after an expiry or a chip reset (POR starts expired): every
        // "reset by WATCHDOG" field is back at its default. Remember before the kick:
        // once a write lands WD_STAT reads 0 again.
        reapply_pending_ = true;
        os::warn("Charger: BQ25792 watchdog expired or chip reset, re-applying configuration\n");
        auto r = check("kick watchdog", driver_.kick_watchdog());
        if (!r) {
            return r;
        }
    }
    if (reapply_pending_) {
        return reapply_all_locked();
    }
    return {};
}

std::expected<void, ControllerError> Charger::check_watchdog_locked() noexcept {
    // Called before writes from any thread: the write would clear WD_STAT and hide the reset.
    if (!driver_.is_initialized()) {
        return std::unexpected(ControllerError::BQ25792_NotInitialized);
    }
    auto expired = check("read watchdog status", driver_.is_watchdog_expired());
    if (!expired) {
        return std::unexpected(expired.error());
    }
    return handle_watchdog_locked(*expired);
}

std::expected<void, ControllerError> Charger::reapply_all_locked() noexcept {
    reapply_pending_ = true;   // cleared only when everything below went through

    // ADC_EN is reset by the watchdog; channel settings only by POR/REG_RST.
    auto adc = check("configure ADC", driver_.configure_adc());
    if (!adc) {
        return adc;
    }
    auto policy = apply_policy_bits_locked();
    if (!policy) {
        return policy;
    }
    // ICHG and EN_CHG are reset by the watchdog.
    auto settings = apply_settings_locked(settings_);
    if (!settings) {
        settings_dirty_ = true;
        return settings;
    }
    settings_dirty_ = false;
    // VAC1_PD_EN survives a watchdog expiry and an MCU reset: write the wanted state
    // (off unless a discharge is running) so a stale 60 Ohm load never stays on.
    auto pulldown = check("set VAC1 pull-down", driver_.set_vac1_pulldown(vac1_discharge_));
    if (!pulldown) {
        return pulldown;
    }
    // VBUS_PD_EN too (a power off that did not finish before an MCU reset).
    auto vbus_pulldown = check("set VBUS pull-down", driver_.set_vbus_pulldown(vbus_discharge_));
    if (!vbus_pulldown) {
        return vbus_pulldown;
    }
    // VOTG, IOTG and EN_OTG are reset by the watchdog; a POR also resets the mux and
    // IINDPM. Forget what was written and write the whole target again.
    written_ = PowerPathWritten{};
    mux_seen_known_ = false;
    if (has_target_) {
        auto power_path = apply_power_path_locked();
        if (!power_path) {
            return power_path;
        }
    }
    reapply_pending_ = false;
    reapplied_unreported_ = true;
    return {};
}

std::expected<void, ControllerError> Charger::apply_policy_bits_locked() noexcept {
    // Take over the limit in use (REG19) before removing the ILIM_HIZ clamp, so clearing
    // EN_EXTILIM never raises the input current by itself. REG19 reads 0 before an input
    // was qualified; IINDPM is "clamped low" and would ignore that write, so skip it.
    auto in_use = check("read input current limit in use", driver_.get_effective_input_current_limit());
    if (!in_use) {
        return std::unexpected(in_use.error());
    }
    if (*in_use >= kIindpmMinA) {
        written_.iindpm_known = false;
        auto r = check("set input current limit", driver_.set_input_current_limit(std::min(*in_use, kIindpmMaxA)));
        if (!r) {
            return r;
        }
    }
    // ILIM_HIZ sits on a 127k/100k divider from REGN: it would clamp IINDPM near 1.4 A.
    auto ilim = check("disable ILIM_HIZ clamp", driver_.set_ext_ilim_enabled(false));
    if (!ilim) {
        return ilim;
    }
    // D+/D- are not connected: detection at VAC1 plug-in would rewrite IINDPM (HVDCP_EN too).
    auto detection = check("disable D+/D- detection", driver_.set_auto_input_detection(false));
    if (!detection) {
        return detection;
    }
    // Battery discharge current is only measured with EN_IBAT=1 (OTG / battery checks).
    auto ibat = check("enable IBAT discharge sensing", driver_.set_ibat_discharge_sense(true));
    if (!ibat) {
        return ibat;
    }
    // Q3 on SDRV: without SFET_PRESENT the chip locks SDRV_CTRL at 00 (no ship mode).
    return check("set ship FET present", driver_.set_ship_fet_present(CHARGER_SHIP_FET_PRESENT != 0));
}

std::expected<void, ControllerError> Charger::apply_settings_locked(const Settings& s) noexcept {
    if (s.has_charge_voltage_limit) {
        auto r = check("set charge voltage limit", driver_.set_charge_voltage_limit(s.charge_voltage_limit));
        if (!r) {
            return r;
        }
    }
    if (s.has_charge_current_limit) {
        auto r = check("set charge current limit", driver_.set_charge_current_limit(s.charge_current_limit));
        if (!r) {
            return r;
        }
    }
    if (s.has_enabled) {
        auto r = check("set enabled", driver_.set_enabled(s.enabled));
        if (!r) {
            return r;
        }
    }
    return {};
}

// =============================================================================
// Input power path
// =============================================================================

std::expected<void, ControllerError> Charger::poll_inputs(ChargerInputPoll& poll, bool full) noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        poll = ChargerInputPoll{};
        poll.settings = input_settings_;
        if (!driver_.is_initialized()) {
            // Not logged here (every 20 ms): service() retries the init and logs.
            return std::unexpected(ControllerError::BQ25792_NotInitialized);
        }

        auto status = check("poll input status", driver_.get_input_status());
        if (!status) {
            return std::unexpected(status.error());
        }
        auto r = handle_watchdog_locked(status->wd_expired);
        if (!r) {
            return r;
        }
        auto volts = check("poll input voltages", driver_.get_input_voltages());
        if (!volts) {
            return std::unexpected(volts.error());
        }

        poll.vbus_present = status->vbus_present;
        poll.ac1_present = status->ac1_present;
        poll.ac2_present = status->ac2_present;
        poll.power_good = status->power_good;
        poll.poor_source = status->poor_source;
        poll.in_vindpm = status->vindpm;
        poll.in_iindpm = status->iindpm;
        poll.vac1_mv = volts->vac1_mv;
        poll.vac2_mv = volts->vac2_mv;
        poll.vbat_mv = volts->vbat_mv;

        if (full) {
            auto mux = check("poll input mux", driver_.get_mux_state());
            if (!mux) {
                return std::unexpected(mux.error());
            }
            auto bat = check("poll battery status", driver_.get_battery_otg_status());
            if (!bat) {
                return std::unexpected(bat.error());
            }
            poll.full = true;
            poll.battery_present = bat->battery_present;
            poll.otg_enabled = mux->en_otg;
            poll.en_acdrv1 = mux->en_acdrv1;
            poll.en_acdrv2 = mux->en_acdrv2;
            poll.dis_acdrv = mux->dis_acdrv;
            poll.vbat_otg_low = bat->vbat_otg_low;
            poll.ts_cold = bat->ts_cold;
            poll.ts_hot = bat->ts_hot;
            poll.otg_ovp = bat->otg_ovp;
            poll.otg_uvp = bat->otg_uvp;
            poll.otg_fault = bat->otg_ovp || bat->otg_uvp;
            poll.vac1_ovp = bat->vac1_ovp;
            poll.vac2_ovp = bat->vac2_ovp;

            // The chip swaps inputs on its own (failover) and rewrites EN_ACDRV1/2. When the
            // mux moved away from our last selection, forget it so the next
            // apply_power_path() selects the target again. Only on a change: a selection
            // the chip did not take (input not valid) is not retried every poll.
            const bool moved = mux_seen_known_ &&
                               (mux->en_acdrv1 != mux_seen_acdrv1_ || mux->en_acdrv2 != mux_seen_acdrv2_);
            if (moved && written_.input_known && !mux_selects(written_.input, *mux)) {
                written_.input_known = false;
                os::info("Charger: input mux changed by the charger (ACDRV1=%d ACDRV2=%d)\n",
                         mux->en_acdrv1 ? 1 : 0, mux->en_acdrv2 ? 1 : 0);
            }
            mux_seen_known_ = true;
            mux_seen_acdrv1_ = mux->en_acdrv1;
            mux_seen_acdrv2_ = mux->en_acdrv2;
        }

        poll.watchdog_reapplied = reapplied_unreported_;
        reapplied_unreported_ = false;
        return {};
    });
}

Charger::PowerPathPlan Charger::plan_power_path_locked() const noexcept {
    const PowerPathTarget& t = target_;
    PowerPathPlan p{};
    p.iindpm_ma = to_milli_steps(t.iindpm_a, kIindpmMinA, kIindpmMaxA, 10);
    p.vindpm_mv = to_milli_steps(t.vindpm_v, kVindpmMinV, kVindpmMaxV, 100);
    p.iindpm = !written_.iindpm_known || written_.iindpm_ma != p.iindpm_ma;
    p.vindpm = !written_.vindpm_known || written_.vindpm_mv != p.vindpm_mv;
    // Lower (or unknown) limits go before the mux change, higher ones after it, so the
    // input never runs above the new limits, e.g. dock 3 A -> USB-C 0.5 A or 20 V -> 5 V.
    p.iindpm_first = p.iindpm && (!written_.iindpm_known || p.iindpm_ma < written_.iindpm_ma);
    p.vindpm_first = p.vindpm && (!written_.vindpm_known || p.vindpm_mv < written_.vindpm_mv);

    if (t.otg) {
        p.votg_mv = to_milli_steps(t.otg_voltage_v, kVotgMinV, kVotgMaxV, 10);
        p.iotg_ma = to_milli_steps(t.otg_current_a, kIotgMinA, kIotgMaxA, 40);
        p.votg = !written_.votg_known || written_.votg_mv != p.votg_mv;
        p.iotg = !written_.iotg_known || written_.iotg_ma != p.iotg_ma;
        p.otg_on = !written_.otg_known || !written_.otg;
        // OTG drives VAC1: the converter waits for EN_ACDRV1=1 (Table 8-8, txt:4025).
        // Written after every EN_OTG 0->1 so the order is always EN_OTG, then ACDRV1.
        p.mux_input = ChargerInputSource_CHARGER_INPUT_AC1;
        p.mux = p.otg_on || !written_.input_known || written_.input != ChargerInputSource_CHARGER_INPUT_AC1;
    } else {
        p.otg_off = !written_.otg_known || written_.otg;
        p.mux_input = t.input;
        p.mux = is_forced_input(t.input) && (!written_.input_known || written_.input != t.input);
    }
    return p;
}

std::expected<void, ControllerError> Charger::write_iindpm_locked(uint32_t ma) noexcept {
    written_.iindpm_known = false;   // unknown until the write is confirmed
    auto r = check("set input current limit", driver_.set_input_current_limit(milli_to_unit(ma)));
    if (!r) {
        return r;
    }
    written_.iindpm_known = true;
    written_.iindpm_ma = ma;
    return {};
}

std::expected<void, ControllerError> Charger::write_vindpm_locked(uint32_t mv) noexcept {
    written_.vindpm_known = false;
    auto r = check("set input voltage limit", driver_.set_input_voltage_limit(milli_to_unit(mv)));
    if (!r) {
        return r;
    }
    written_.vindpm_known = true;
    written_.vindpm_mv = mv;
    return {};
}

std::expected<void, ControllerError> Charger::write_mux_locked(ChargerInputSource input) noexcept {
    // DIS_ACDRV=0, then EN_ACDRV1/EN_ACDRV2 in a single REG13 write (the chip swaps paths).
    written_.input_known = false;
    auto r = check("select input",
                   driver_.set_input_source(static_cast<bq25792::ChargerInputSource>(input)));
    if (!r) {
        return r;
    }
    written_.input_known = true;
    written_.input = input;
    return {};
}

std::expected<void, ControllerError> Charger::apply_power_path_locked() noexcept {
    const PowerPathPlan p = plan_power_path_locked();
    target_dirty_ = true;   // until every write below went through

    if (p.iindpm_first) {
        auto r = write_iindpm_locked(p.iindpm_ma);
        if (!r) return r;
    }
    if (p.vindpm_first) {
        auto r = write_vindpm_locked(p.vindpm_mv);
        if (!r) return r;
    }
    if (p.otg_off) {
        // Before any mux change, so OTG never drives the newly selected input.
        written_.otg_known = false;
        auto r = check("disable OTG", driver_.set_otg_enabled(false));
        if (!r) return r;
        written_.otg_known = true;
        written_.otg = false;
    }
    if (!target_.otg) {
        if (p.mux) {
            auto r = write_mux_locked(p.mux_input);
            if (!r) return r;
        } else if (!is_forced_input(target_.input)) {
            // AUTO leaves the mux to the chip: what it selects is no longer ours.
            written_.input_known = false;
        }
    }
    if (p.iindpm && !p.iindpm_first) {
        auto r = write_iindpm_locked(p.iindpm_ma);
        if (!r) return r;
    }
    if (p.vindpm && !p.vindpm_first) {
        auto r = write_vindpm_locked(p.vindpm_mv);
        if (!r) return r;
    }
    if (target_.otg) {
        if (p.votg) {
            written_.votg_known = false;
            auto r = check("set OTG voltage", driver_.set_otg_voltage(milli_to_unit(p.votg_mv)));
            if (!r) return r;
            written_.votg_known = true;
            written_.votg_mv = p.votg_mv;
        }
        if (p.iotg) {
            written_.iotg_known = false;
            auto r = check("set OTG current limit", driver_.set_otg_current_limit(milli_to_unit(p.iotg_ma)));
            if (!r) {
                return r;
            }
            written_.iotg_known = true;
            written_.iotg_ma = p.iotg_ma;
        }
        if (p.otg_on) {
            written_.otg_known = false;
            auto r = check("enable OTG", driver_.set_otg_enabled(true));
            if (!r) return r;
            written_.otg_known = true;
            written_.otg = true;
        }
        if (p.mux) {
            auto r = write_mux_locked(ChargerInputSource_CHARGER_INPUT_AC1);
            if (!r) return r;
        }
    }
    target_dirty_ = false;
    return {};
}

std::expected<void, ControllerError> Charger::apply_power_path(const PowerPathTarget& target) noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        target_ = target;
        has_target_ = true;
        target_dirty_ = true;
        if (!driver_.is_initialized()) {
            return std::unexpected(ControllerError::BQ25792_NotInitialized);  // applied after service() inits
        }
        if (!plan_power_path_locked().any()) {
            target_dirty_ = false;
            return {};
        }
        // A pending watchdog reset re-applies the whole (new) target; then nothing is left.
        auto r = check_watchdog_locked();
        if (!r) {
            return r;
        }
        return apply_power_path_locked();
    });
}

std::expected<void, ControllerError> Charger::set_vac1_discharge(bool on) noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        vac1_discharge_ = on;   // re-applied after a chip reset
        auto r = check_watchdog_locked();
        if (!r) {
            return r;
        }
        return check(on ? "enable VAC1 pull-down" : "disable VAC1 pull-down", driver_.set_vac1_pulldown(on));
    });
}

std::expected<void, ControllerError> Charger::set_vbus_discharge(bool on) noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        vbus_discharge_ = on;   // re-applied after a chip reset
        auto r = check_watchdog_locked();
        if (!r) {
            return r;
        }
        return check(on ? "enable VBUS pull-down" : "disable VBUS pull-down", driver_.set_vbus_pulldown(on));
    });
}

// =============================================================================
// Ship mode
// =============================================================================

std::expected<bool, ControllerError> Charger::enter_ship_mode() noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<bool, ControllerError> {
        auto r = check_watchdog_locked();
        if (!r) {
            return std::unexpected(r.error());
        }
        auto set = check("enter ship mode", driver_.set_ship_fet_mode(bq25792::ShipFetMode::Ship, true));
        if (!set) {
            return std::unexpected(set.error());
        }
        auto mode = check("read ship FET mode", driver_.get_ship_fet_mode());
        if (!mode) {
            return std::unexpected(mode.error());
        }
        return *mode == bq25792::ShipFetMode::Ship;
    });
}

std::expected<void, ControllerError> Charger::cancel_ship_mode() noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        auto r = check_watchdog_locked();
        if (!r) {
            return r;
        }
        return check("cancel ship mode", driver_.set_ship_fet_mode(bq25792::ShipFetMode::Idle, true));
    });
}

std::expected<bool, ControllerError> Charger::ship_mode_pending() const noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<bool, ControllerError> {
        auto mode = check("read ship FET mode", driver_.get_ship_fet_mode());
        if (!mode) {
            return std::unexpected(mode.error());
        }
        return *mode == bq25792::ShipFetMode::Ship;
    });
}

// =============================================================================
// Control
// =============================================================================

std::expected<void, ControllerError> Charger::set_enabled(bool enable) noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        settings_.has_enabled = true;
        settings_.enabled = enable;
        auto r = check_watchdog_locked();
        if (r) {
            r = check("set enabled", driver_.set_enabled(enable));
        }
        settings_dirty_ = settings_dirty_ || !r;
        return r;
    });
}

std::expected<bool, ControllerError> Charger::is_enabled() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read enabled state", driver_.is_enabled()); });
}

std::expected<void, ControllerError> Charger::set_input_source(ChargerInputSource source) noexcept {
    if (!valid_input_source(source)) {
        return std::unexpected(ControllerError::BQ25792_InvalidParameter);
    }
    // Stored only (PowerPath selects the input); the bus lock guards the settings.
    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        input_settings_.source_mode = source;
        return {};
    });
}

std::expected<ChargerInputSource, ControllerError> Charger::get_input_source() const noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<ChargerInputSource, ControllerError> {
        auto r = check("read input source", driver_.get_input_source());
        if (!r) {
            return std::unexpected(r.error());
        }
        return static_cast<ChargerInputSource>(*r);
    });
}

std::expected<void, ControllerError> Charger::set_config(const ChargerConfig& config) noexcept {
    ChargerConfigRequest req = ChargerConfigRequest_init_zero;
    req.has_charge_voltage_limit = true;
    req.charge_voltage_limit = config.charge_voltage_limit;
    req.has_charge_current_limit = true;
    req.charge_current_limit = config.charge_current_limit;
    // input_*_limit report the values in use; the explicit floor/cap fields, when
    // present, are the host settings and win (get_config() -> set_config() round trip).
    req.has_input_voltage_limit = true;
    req.input_voltage_limit = config.has_input_voltage_floor ? config.input_voltage_floor : config.input_voltage_limit;
    req.has_input_current_limit = true;
    req.input_current_limit = config.has_input_current_cap ? config.input_current_cap : config.input_current_limit;
    req.has_dock_input_current_limit = config.has_dock_input_current_limit;
    req.dock_input_current_limit = config.dock_input_current_limit;
    return set_config(req);
}

void Charger::store_input_settings_locked(const ChargerConfigRequest& config) noexcept {
    if (config.has_input_voltage_limit) {
        input_settings_.voltage_floor_v = config.input_voltage_limit;
    }
    if (config.has_input_current_limit) {
        input_settings_.current_cap_a = config.input_current_limit;
    }
    if (config.has_dock_input_current_limit) {
        input_settings_.dock_limit_a = config.dock_input_current_limit;
    }
    if (config.has_input_source) {
        input_settings_.source_mode = config.input_source;
    }
}

std::expected<void, ControllerError> Charger::set_config(const ChargerConfigRequest& config) noexcept {
    // Validate every present field before touching anything, so a bad request never
    // leaves the charger half-configured.
    if ((config.has_charge_voltage_limit && !in_range(config.charge_voltage_limit, 3.0f, 18.8f)) ||
        (config.has_charge_current_limit && !in_range(config.charge_current_limit, 0.05f, 5.0f)) ||
        (config.has_input_voltage_limit && !in_range(config.input_voltage_limit, kVindpmMinV, kVindpmMaxV)) ||
        (config.has_input_current_limit && !in_range(config.input_current_limit, kIindpmMinA, kIindpmMaxA)) ||
        (config.has_dock_input_current_limit &&
         !in_range(config.dock_input_current_limit, kIindpmMinA, kIindpmMaxA)) ||
        (config.has_input_source && !valid_input_source(config.input_source))) {
        os::error("Charger: rejected out-of-range configuration request\n");
        return std::unexpected(ControllerError::BQ25792_InvalidParameter);
    }

    Settings delta{};
    if (config.has_charge_voltage_limit) {
        delta.has_charge_voltage_limit = true;
        delta.charge_voltage_limit = config.charge_voltage_limit;
    }
    if (config.has_charge_current_limit) {
        delta.has_charge_current_limit = true;
        delta.charge_current_limit = config.charge_current_limit;
    }
    if (config.has_enabled) {
        delta.has_enabled = true;
        delta.enabled = config.enabled;
    }
    const bool writes = delta.has_charge_voltage_limit || delta.has_charge_current_limit || delta.has_enabled;

    return i2c_internal_bus_transaction([&]() -> std::expected<void, ControllerError> {
        // Input settings feed the PowerPath decision only: stored, never written here.
        store_input_settings_locked(config);
        if (!writes) {
            return {};
        }

        // Remember the request even if the write fails part-way: service() re-applies it.
        if (delta.has_charge_voltage_limit) {
            settings_.has_charge_voltage_limit = true;
            settings_.charge_voltage_limit = delta.charge_voltage_limit;
        }
        if (delta.has_charge_current_limit) {
            settings_.has_charge_current_limit = true;
            settings_.charge_current_limit = delta.charge_current_limit;
        }
        if (delta.has_enabled) {
            settings_.has_enabled = true;
            settings_.enabled = delta.enabled;
        }
        auto r = check_watchdog_locked();
        if (r) {
            r = apply_settings_locked(delta);
        }
        settings_dirty_ = settings_dirty_ || !r;
        return r;
    });
}

// =============================================================================
// Read-back helpers (caller holds the bus lock)
// =============================================================================

std::expected<void, ControllerError> Charger::read_status_locked(ChargerStatus& status) const noexcept {
    auto enabled = check("read enabled state", driver_.is_enabled());
    if (!enabled) return std::unexpected(enabled.error());
    auto plugged = check("read VBUS presence", driver_.is_plugged_in());
    if (!plugged) return std::unexpected(plugged.error());
    auto charge = check("read charge status", driver_.get_charge_status());
    if (!charge) return std::unexpected(charge.error());
    auto vbus = check("read VBUS status", driver_.get_vbus_status());
    if (!vbus) return std::unexpected(vbus.error());
    auto battery = check("read battery presence", driver_.is_battery_present());
    if (!battery) return std::unexpected(battery.error());
    auto fault = check("read fault status", driver_.is_fault_present());
    if (!fault) return std::unexpected(fault.error());
    auto source = check("read input source", driver_.get_input_source());
    if (!source) return std::unexpected(source.error());
    uint8_t regs[13];   // REG1B..REG27: CHARGER_STATUS_0..4, FAULT_STATUS_0..1, flags
    auto r = check("read status block", driver_.read_status_block(regs));
    if (!r) return r;

    status.enabled = *enabled;
    status.plugged_in = *plugged;
    status.charge_status = static_cast<ChargeStatus>(*charge);
    status.vbus_status = static_cast<VBUSStatus>(*vbus);
    status.battery_present = *battery;
    status.fault_present = *fault;
    status.input_source = static_cast<ChargerInputSource>(*source);
    status.timestamp_ms = last_update_ms_;
    status.has_status_flags = true;
    status.status_flags = uint32_t{regs[0]} | (uint32_t{regs[2]} << 8) | (uint32_t{regs[3]} << 16) |
                          (uint32_t{regs[4]} << 24);
    status.has_fault_flags = true;
    status.fault_flags = uint32_t{regs[5]} | (uint32_t{regs[6]} << 8);
    return {};
}

std::expected<void, ControllerError> Charger::read_measurements_locked(ChargerMeasurements& m) const noexcept {
    auto adc = check("read ADC", driver_.get_adc_measurements());
    if (!adc) {
        return std::unexpected(adc.error());
    }
    m.vbat_voltage = adc->vbat_v;
    m.ibus_current = adc->ibus_a;
    m.vbus_voltage = adc->vbus_v;
    m.vsys_voltage = adc->vsys_v;
    m.has_ibat_current = true;
    m.ibat_current = adc->ibat_a;
    m.has_vac1_voltage = true;
    m.vac1_voltage = adc->vac1_v;
    m.has_vac2_voltage = true;
    m.vac2_voltage = adc->vac2_v;
    return {};
}

std::expected<void, ControllerError> Charger::read_config_locked(ChargerConfig& config) const noexcept {
    auto vlim = check("read charge voltage limit", driver_.get_charge_voltage_limit());
    if (!vlim) return std::unexpected(vlim.error());
    auto ilim = check("read charge current limit", driver_.get_charge_current_limit());
    if (!ilim) return std::unexpected(ilim.error());
    auto ivlim = check("read input voltage limit", driver_.get_input_voltage_limit());
    if (!ivlim) return std::unexpected(ivlim.error());
    auto iilim = check("read input current limit", driver_.get_input_current_limit());
    if (!iilim) return std::unexpected(iilim.error());

    config.charge_voltage_limit = *vlim;
    config.charge_current_limit = *ilim;
    config.input_voltage_limit = *ivlim;    // VINDPM in use
    config.input_current_limit = *iilim;    // IINDPM in use
    config.has_dock_input_current_limit = true;
    config.dock_input_current_limit = input_settings_.dock_limit_a;
    config.has_input_current_cap = true;
    config.input_current_cap = input_settings_.current_cap_a;
    config.has_input_voltage_floor = true;
    config.input_voltage_floor = input_settings_.voltage_floor_v;
    config.has_input_source_mode = true;
    config.input_source_mode = input_settings_.source_mode;
    return {};
}

// =============================================================================
// Read-back (public)
// =============================================================================

std::expected<void, ControllerError> Charger::get_status(ChargerStatus& status) const noexcept {
    return i2c_internal_bus_transaction([&] { return read_status_locked(status); });
}

std::expected<void, ControllerError> Charger::get_measurements(ChargerMeasurements& measurements) const noexcept {
    return i2c_internal_bus_transaction([&] { return read_measurements_locked(measurements); });
}

std::expected<void, ControllerError> Charger::get_config(ChargerConfig& config) const noexcept {
    return i2c_internal_bus_transaction([&] { return read_config_locked(config); });
}

std::expected<void, ControllerError> Charger::get_status(SystemStatus& status) const noexcept {
    auto r = i2c_internal_bus_transaction([&] { return read_status_locked(status.charger); });
    status.has_charger = r.has_value();
    return r;
}

std::expected<void, ControllerError> Charger::get_measurements(SystemStatus& status) const noexcept {
    auto r = i2c_internal_bus_transaction([&] { return read_measurements_locked(status.charger_measurements); });
    status.has_charger_measurements = r.has_value();
    return r;
}

std::expected<void, ControllerError> Charger::get_config(SystemStatus& status) const noexcept {
    auto r = i2c_internal_bus_transaction([&] { return read_config_locked(status.charger_config); });
    status.has_charger_config = r.has_value();
    return r;
}

std::expected<void, ControllerError> Charger::get_full_status(SystemStatus& status) const noexcept {
    auto s = get_status(status);
    auto m = get_measurements(status);
    auto c = get_config(status);
    if (!s) return s;
    if (!m) return m;
    return c;
}

// =============================================================================
// Update notification
// =============================================================================

bool Charger::has_updates() const noexcept {
    return update_sem_.try_acquire();
}

bool Charger::wait_for_update(uint32_t timeout_ms) const noexcept {
    if (timeout_ms == 0) {
        update_sem_.acquire();
        return true;
    }
    return update_sem_.try_acquire_for(timeout_ms);
}

void Charger::trigger_update() noexcept {
    update_sem_.release();
}

// =============================================================================
// Single-value accessors
// =============================================================================

bool Charger::is_initialized() const noexcept {
    return driver_.is_initialized();
}

std::expected<bool, ControllerError> Charger::is_plugged_in() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read VBUS presence", driver_.is_plugged_in()); });
}

std::expected<bool, ControllerError> Charger::is_battery_present() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read battery presence", driver_.is_battery_present()); });
}

std::expected<bool, ControllerError> Charger::is_fault_present() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read fault status", driver_.is_fault_present()); });
}

std::expected<ChargeStatus, ControllerError> Charger::get_charge_status() const noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<ChargeStatus, ControllerError> {
        auto r = check("read charge status", driver_.get_charge_status());
        if (!r) return std::unexpected(r.error());
        return static_cast<ChargeStatus>(*r);
    });
}

std::expected<VBUSStatus, ControllerError> Charger::get_vbus_status() const noexcept {
    return i2c_internal_bus_transaction([&]() -> std::expected<VBUSStatus, ControllerError> {
        auto r = check("read VBUS status", driver_.get_vbus_status());
        if (!r) return std::unexpected(r.error());
        return static_cast<VBUSStatus>(*r);
    });
}

std::expected<float, ControllerError> Charger::get_vbus_voltage() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read VBUS voltage", driver_.get_vbus()); });
}

std::expected<float, ControllerError> Charger::get_vsys_voltage() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read VSYS voltage", driver_.get_vsys()); });
}

std::expected<float, ControllerError> Charger::get_vbat_voltage() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read VBAT voltage", driver_.get_vbat()); });
}

std::expected<float, ControllerError> Charger::get_ibus_current() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read IBUS current", driver_.get_ibus()); });
}

std::expected<float, ControllerError> Charger::get_ibat_current() const noexcept {
    return i2c_internal_bus_transaction([&] { return check("read IBAT current", driver_.get_ibat()); });
}

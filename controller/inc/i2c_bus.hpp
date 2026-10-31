#ifndef __I2C_BUS_HPP__
#define __I2C_BUS_HPP__

#include "mutex.hpp"
#include "error.hpp"
#include <expected>
#include <mutex>
#include <type_traits>

/**
 * @brief Mutex serialising all traffic on the internal I2C bus (g_board_i2c_internal).
 *
 * The BQ25792 charger and the TPS55289 regulator share one I2C peripheral and are
 * accessed from different threads (message handler, power service). A HAL I2C
 * handle is not re-entrant, so every driver call on that bus must hold this lock.
 * The mutex is not recursive: lock it only in public entry points.
 */
os::mutex& i2c_internal_bus_mutex();

/**
 * @brief Recover a stuck internal I2C bus. Caller must hold i2c_internal_bus_mutex().
 *
 * De-initialises the I2C peripheral, clocks SCL (up to 9 pulses) until a slave
 * holding SDA low releases it, generates a STOP condition and re-initialises the
 * peripheral including its analog/digital filter settings.
 *
 * @return true if SDA and SCL are both high afterwards and the peripheral is ready
 */
bool i2c_internal_bus_recover() noexcept;

/**
 * @brief Number of bus recoveries performed since boot (diagnostics)
 */
uint32_t i2c_internal_bus_recovery_count() noexcept;

/**
 * @brief Run an operation on the internal I2C bus.
 *
 * Holds the bus lock for the whole operation. If the operation fails with a bus
 * fault (see is_i2c_bus_fault()) the bus is recovered and the operation retried
 * once. The operation must therefore be safe to repeat: register writes and
 * read-modify-write sequences on the BQ25792/TPS55289 are.
 *
 * @param op callable returning std::expected<T, ControllerError>
 */
template <typename Op>
auto i2c_internal_bus_transaction(Op&& op) -> std::invoke_result_t<Op&> {
    std::lock_guard lock(i2c_internal_bus_mutex());
    auto result = op();
    if (!result && is_i2c_bus_fault(result.error()) && i2c_internal_bus_recover()) {
        result = op();
    }
    return result;
}

#endif

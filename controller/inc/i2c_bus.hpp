#ifndef __I2C_BUS_HPP__
#define __I2C_BUS_HPP__

#include "mutex.hpp"

/**
 * @brief Mutex serialising all traffic on the internal I2C bus (g_board_i2c_internal).
 *
 * The BQ25792 charger and the TPS55289 regulator share one I2C peripheral and are
 * accessed from different threads (message handler, charger service). A HAL I2C
 * handle is not re-entrant, so every driver call on that bus must hold this lock.
 * The mutex is not recursive: lock it only in public entry points.
 */
os::mutex& i2c_internal_bus_mutex();

#endif

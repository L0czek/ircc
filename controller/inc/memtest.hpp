#ifndef __MEMTEST_HPP__
#define __MEMTEST_HPP__

#include <cstdint>
#include <utility>

namespace memtest {

/**
 * @brief Run PSRAM memory tests
 * @return true if all tests passed, false otherwise
 * 
 * Before calling this function, ensure:
 * 1. PSRAM driver is initialized and g_psram_driver is set
 * 2. QSPI and DMA are properly configured
 */
bool run();

/**
 * @brief Memory training - find the fastest working QSPI clock speed
 * Tests prescaler values from fastest (256 division) to slowest (2 division)
 * and returns the fastest prescaler that passes all tests.
 * 
 * @return Prescaler value (2, 4, 8, 16, 32, 64, 128, or 256) that passed,
 *         or 0 if no prescaler works
 */
uint32_t train();

} // namespace memtest

#endif // __MEMTEST_HPP__

#ifndef UI_FORMAT_HPP
#define UI_FORMAT_HPP

#include <cstddef>
#include <cstdint>

namespace ui {

/**
 * Number formatting without printf float support: integer arithmetic, round half away
 * from zero. All functions write a NUL-terminated string (truncated to n - 1 chars) and
 * return its length.
 */

/** @brief "12.35", "-0.5"; plus: "+1.20" for positive values */
size_t format_fixed(char* buf, size_t n, float value, int decimals, bool plus = false) noexcept;

/** @brief format_fixed() followed by the unit: "12.35V" */
size_t format_value(char* buf, size_t n, float value, int decimals, const char* unit, bool plus = false) noexcept;

/** @brief Value with an SI prefix and `digits` significant digits: 125000 Hz -> "125kHz",
 *         0.0042 A -> "4.20mA" */
size_t format_si(char* buf, size_t n, float value, const char* unit, int digits = 3) noexcept;

/** @brief "0:42", "12:05", "1:02:03" */
size_t format_duration(char* buf, size_t n, uint32_t seconds) noexcept;

/** @brief Unsigned integer */
size_t format_uint(char* buf, size_t n, uint32_t value) noexcept;

/** @brief Append src to buf (size n); returns the new length */
size_t append(char* buf, size_t n, const char* src) noexcept;

} // namespace ui

#endif

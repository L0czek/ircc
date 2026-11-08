// Drawing helpers shared by the screens (private to controller/ui/src)
#ifndef UI_WIDGETS_HPP
#define UI_WIDGETS_HPP

#include "ui/gfx.hpp"
#include "ui/ui.hpp"

namespace ui {

/** @brief Label left, value right on one 10 px row of the body font */
void draw_kv(Canvas& c, int y, const char* label, const char* value, Gray value_gray = gray::kValue) noexcept;

/** @brief Time series chart: label + latest value on top, min/max on the right, line plot */
void draw_chart(Canvas& c, const Rect& r, const Series& s, const char* label, const char* unit, int decimals) noexcept;

/** @brief Page indicator dots, centred in r */
void draw_dots(Canvas& c, const Rect& r, int count, int current) noexcept;

const char* usb_state_text(UsbPdState s) noexcept;
const char* usb_role_text(UsbPdPowerRole r) noexcept;
const char* usb_source_text(UsbPdSourceState s) noexcept;
const char* usb_path_text(UsbPdSourcePath p) noexcept;
const char* input_text(const SystemStatus& s) noexcept;
const char* regulator_mode_text(OutputRegulatorOperatingMode m) noexcept;

// Screens opened from other screens
Screen& applet_run_screen(size_t applet) noexcept;
Screen& applets_screen() noexcept;

} // namespace ui

#endif

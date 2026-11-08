#ifndef UI_SCREENS_HPP
#define UI_SCREENS_HPP

#include "ui/ui.hpp"

namespace ui {

/** @brief Dashboard (home) and everything reachable from it; static objects */
Screen& home_screen() noexcept;

/** @brief Screens opened directly (simulator scripts, tests) */
Screen& main_menu_screen() noexcept;
Screen& input_test_screen() noexcept;
Screen& display_test_screen() noexcept;
Screen& power_off_screen() noexcept;

} // namespace ui

#endif

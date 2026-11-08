# Front panel UI sources (platform independent: firmware and host simulator/tests)
set(PANEL_UI_DIR ${CMAKE_CURRENT_LIST_DIR})
set(PANEL_UI_SOURCES
    ${PANEL_UI_DIR}/src/gfx.cc
    ${PANEL_UI_DIR}/src/fonts.cc
    ${PANEL_UI_DIR}/src/icons.cc
    ${PANEL_UI_DIR}/src/format.cc
    ${PANEL_UI_DIR}/src/input.cc
    ${PANEL_UI_DIR}/src/ui.cc
    ${PANEL_UI_DIR}/src/menu.cc
    ${PANEL_UI_DIR}/src/menus.cc
    ${PANEL_UI_DIR}/src/dashboard.cc
    ${PANEL_UI_DIR}/src/power_off.cc
    ${PANEL_UI_DIR}/../src/applet_control.cc
)
set(PANEL_UI_INC ${PANEL_UI_DIR}/inc ${PANEL_UI_DIR}/../inc)

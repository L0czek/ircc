add_library(aux_pwm_applet
    aux_pwm_applet.cc
)

target_include_directories(aux_pwm_applet PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../inc
    ${BOARD_HEADERS_INC}
    ${OS_API_INC}
    ${CONTROLLER_INC}
)

target_link_libraries(aux_pwm_applet PUBLIC ctl)

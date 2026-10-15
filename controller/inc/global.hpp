#ifndef __GLOBAL_HPP__
#define __GLOBAL_HPP__

#include "board.h"
#include "message_handler.hpp"
#include "charger.hpp"
#include "output_voltage_regulator.hpp"
#include "dac_controller.hpp"
#include "aux_pwm_applet.hpp"

// Global objects
extern MessageHandler message_handler;
extern Charger* charger;
extern OutputVoltageRegulator* output_voltage_regulator;
extern DACController* dac_controller;
extern AuxPwmApplet aux_pwm_applet;

#endif

#include <cstdio>
#include <expected>
#include <memory>

#include "ctl.h"
#include "global.hpp"

#include "message_handler.hpp"
#include "charger.hpp"
#include "output_voltage_regulator.hpp"
#include "dac_controller.hpp"
#include "aux_pwm_applet.hpp"
#include "cmsis_os2.h"
#include "config.hpp"
#include "log.hpp"
#include "stm32g4xx_hal_uart.h"
#include "stm32g4xx_hal_uart_ex.h"
#include "thread.hpp"

MessageHandler message_handler;
Charger* charger = nullptr;
OutputVoltageRegulator* output_voltage_regulator = nullptr;
DACController* dac_controller = nullptr;


// Errors are logged (rate limited) inside service(); the loop just keeps going.
static void power_service(uint32_t period_ms) {
    for (;;) {
        if (charger) {
            (void)charger->service();
        }
        if (output_voltage_regulator) {
            (void)output_voltage_regulator->service();
        }
        osDelay(period_ms);
    }
}

extern "C" void controller_system_init(void) {

    os::init(os::LogLevel::Debug);
    os::info("Booting up\n");

    // Initialize charger with I2C handle from board config
    charger = new Charger(g_board_i2c_internal);
    if (!charger->initialize()) {
        os::error("Failed to initialize charger\n");
    }

    // Initialize output voltage regulator with I2C handle from board config
    output_voltage_regulator = new OutputVoltageRegulator(g_board_i2c_internal);
    if (!output_voltage_regulator->initialize()) {
        os::error("Failed to initialize output voltage regulator\n");
    }

    // Initialize DAC controller with DAC handles from board config
    DAC_HandleTypeDef* dac_handles[4] = {
        g_board_dac_ch1,
        g_board_dac_ch2,
        g_board_dac_ch3,
        g_board_dac_ch4
    };
    dac_controller = new DACController(dac_handles);
    if (!dac_controller->initialize()) {
        os::error("Failed to initialize DAC controller\n");
    }

    // Note: applets are started on-demand via applet_command RPC command
    // aux-pwm can be started with: applet_command { applet_name: "aux-pwm", args: ["start"] }

    static osThreadAttr_t message_handler_sender_thread_attr = {
        .name = "message-handler-sender-thread",
        .attr_bits = osThreadDetached,
        .cb_mem = nullptr,
        .cb_size = 0,
        .stack_mem = nullptr,
        .stack_size = 0,
        .priority = osPriorityNormal,
        .tz_module = 0,
        .reserved = 0
    };

    static auto message_handler_sender_thread = os::thread(
        &message_handler_sender_thread_attr,
        &MessageHandler::run_sender,
        std::move(&message_handler)
    );

    static osThreadAttr_t message_handler_receiver_thread_attr = {
        .name = "message-handler-receiver-thread",
        .attr_bits = osThreadDetached,
        .cb_mem = nullptr,
        .cb_size = 0,
        .stack_mem = nullptr,
        .stack_size = 0,
        .priority = osPriorityNormal1,
        .tz_module = 0,
        .reserved = 0
    };

    static auto message_handler_receiver_thread = os::thread(
        &message_handler_receiver_thread_attr,
        &MessageHandler::run_receiver,
        std::move(&message_handler)
    );

    // Power-device housekeeping (shared internal I2C bus):
    //  - BQ25792: kick the I2C watchdog (40 s default; it resets ICHG/EN_CHG/ADC
    //    when it expires), re-apply settings after an expiry or a failed apply,
    //    retry init if the charger was absent at boot
    //  - TPS55289: retry init, latch clear-on-read SCP/OCP/OVP faults between
    //    host polls
    static osThreadAttr_t power_service_thread_attr = {
        .name = "power-service-thread",
        .attr_bits = osThreadDetached,
        .cb_mem = nullptr,
        .cb_size = 0,
        .stack_mem = nullptr,
        .stack_size = 1024,  // error path formats log messages
        .priority = osPriorityBelowNormal,
        .tz_module = 0,
        .reserved = 0
    };

    static auto power_service_thread = os::thread(
        &power_service_thread_attr,
        &power_service,
        uint32_t{1000}
    );
}

extern "C" void HAL_UART_TxCpltCallback([[maybe_unused]] UART_HandleTypeDef *huart) {
    message_handler.message_tx_isr();
}

extern "C" void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t ) {
    if (HAL_UARTEx_GetRxEventType(huart) == HAL_UART_RXEVENT_IDLE)
        message_handler.message_rx_isr();
}


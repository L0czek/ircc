#include <cstdio>
#include <expected>
#include <memory>

#include "ctl.h"

#include "board.h"
#include "message_handler.hpp"
#include "cmsis_os2.h"
#include "config.hpp"
#include "log.hpp"
#include "stm32g4xx_hal_uart.h"
#include "thread.hpp"

board_config BOARD_CONFIG;
MessageHandler message_handler;


extern "C" void controller_system_init(const board_config *config) {
    BOARD_CONFIG = *config;
    os::init(LOG_LEVEL);
    os::info("Booting up\n");

    MessageHandler *message_handler_ptr = new (&message_handler) MessageHandler;

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

    auto message_handler_sender_thread = os::thread(
        &message_handler_sender_thread_attr,
        &MessageHandler::run_sender,
        std::move(message_handler_ptr)
    );

    static osThreadAttr_t message_handler_receiver_thread_attr = {
        .name = "message-handler-receiver-thread",
        .attr_bits = osThreadDetached,
        .cb_mem = nullptr,
        .cb_size = 0,
        .stack_mem = nullptr,
        .stack_size = 0,
        .priority = osPriorityNormal,
        .tz_module = 0,
        .reserved = 0
    };

    auto message_handler_receiver_thread = os::thread(
        &message_handler_receiver_thread_attr,
        &MessageHandler::run_sender,
        std::move(message_handler_ptr)
    );

    for(;;);
}

extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *) {
    message_handler.message_rx_isr();
}

extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef *) {
    message_handler.message_tx_isr();
}

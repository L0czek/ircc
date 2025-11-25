#include "message_handler.hpp"
#include "cmsis_os2.h"
#include "log.hpp"
#include "message.hpp"
#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "protocol.pb.h"
#include "board.h"
#include "stm32g4xx_hal_crc.h"
#include "stm32g4xx_hal_def.h"
#include "stm32g4xx_hal_uart.h"
#include "stm32g4xx_hal_uart_ex.h"
#include <cstring>
#include <expected>

MessageHandler::MessageHandler() noexcept
: tx_queue(8, nullptr),
    rx_lock(1, 0, nullptr),
    tx_lock(1, 0, nullptr) {
}

void MessageHandler::run_sender() noexcept {
    static message msg = {.raw = {std::byte(0)}};
    std::memcpy(msg.fields.header, MESSAGE_HEADER_ID, sizeof(MESSAGE_HEADER_ID));
    static Response resp = Response_init_zero;

    while (true) {
        pb_ostream_t stream = pb_ostream_from_buffer(
            (pb_byte_t *) msg.fields.data,
            sizeof(msg.fields.data)
        );

        std::uint8_t prio;
        osStatus_t status = tx_queue.get(resp, prio, osWaitForever);

        if (status != osOK)
            continue;

        if (!pb_encode(&stream, Response_fields, &resp)) {
            os::error("Failed to encode message, error: %s\n", PB_GET_ERROR(&stream));
            continue;
        }

        msg.fields.size = stream.bytes_written;
        msg.fields.crc32 = HAL_CRC_Calculate(
            BOARD_CONFIG.crc32,
            (std::uint32_t *) msg.fields.data,
            msg.fields.size
        );

        HAL_StatusTypeDef result = HAL_UART_Transmit_DMA(
            BOARD_CONFIG.message_bus,
            (const std::uint8_t *) msg.raw,
            GET_MESSAGE_SIZE(msg)
        );

        if (result != HAL_OK) {
            os::error("Failed to write mesage to uart\n");
            continue;
        }

        tx_lock.acquire();
    }
}

void MessageHandler::message_tx_isr() noexcept {
    tx_lock.release();
}

void MessageHandler::run_receiver() noexcept {
    Request req = Request_init_zero;
    static message current = { .raw = { std::byte(0) } };
    std::memcpy(current.fields.header, MESSAGE_HEADER_ID, sizeof(MESSAGE_HEADER_ID));

    HAL_UART_Abort(BOARD_CONFIG.message_bus);

    while (true) {
        HAL_StatusTypeDef status = HAL_UARTEx_ReceiveToIdle_DMA(
            BOARD_CONFIG.message_bus,
            (std::uint8_t *) current.raw,
            MESSAGE_SIZE
        );

        if (status != HAL_OK) {
            os::error("Failed to setup uart dma receiver %d\n", status);
            HAL_UART_DMAStop(BOARD_CONFIG.message_bus);
            continue;
        }

        rx_lock.acquire();

        if (std::memcmp(current.fields.header, MESSAGE_HEADER_ID, sizeof(MESSAGE_HEADER_ID))) {
            os::error("Invalid message header ignoring\n");
            continue;
        }

        std::uint32_t crc32 = HAL_CRC_Calculate(
            BOARD_CONFIG.crc32,
            (std::uint32_t *) current.fields.data,
            current.fields.size
        );

        if (crc32 != current.fields.crc32) {
            os::error("Invalid CRC32 value %X != %X\n", crc32, current.fields.crc32);
            continue;
        }

        pb_istream_t stream = pb_istream_from_buffer(
            (const pb_byte_t *) current.fields.data,
            current.fields.size
        );

        if (!pb_decode(&stream, Request_fields, &req)) {
            os::error("Failed to decode request, error: %s\n", PB_GET_ERROR(&stream));
            continue;
        }

        Response resp = handle(req);
        auto result = send(resp);

        if (!result)
            os::error("Failed to send response\n");
    }
}

void MessageHandler::message_rx_isr() noexcept {
    rx_lock.release();
}

Response MessageHandler::handle(const Request &req) const noexcept {
    Response resp = Response_init_zero;

    switch (req.which_command) {
        case Request_ping_tag:
            os::debug("Received ping command\n");
            resp.which_payload = Response_pong_tag;
            break;
    }

    return resp;
}

std::expected<void, osStatus_t> MessageHandler::send(const Response &resp) noexcept {
    osStatus_t status = tx_queue.put(resp, 0, osWaitForever);

    if (status != osOK)
        return std::unexpected(status);

    return {};
}

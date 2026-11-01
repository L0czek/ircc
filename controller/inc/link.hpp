#ifndef __LINK_HPP__
#define __LINK_HPP__

#include "board.h"
#include "error.hpp"
#include "mutex.hpp"
#include "sem.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>

/**
 * @brief Byte-stream links to the host (ctrl UART, USB CDC, aux UART)
 *
 * A Link carries frames (protocol/inc/frame.hpp) but knows nothing about them: it
 * moves bytes. RX bytes land in a ring buffer filled from interrupt context and read
 * by one thread; TX is a blocking send of one buffer at a time (serialised by a mutex),
 * completed from interrupt context.
 */

#if __has_include("usbd_cdc_if.h") && __has_include("usb_device.h")
#define COMM_HAS_USB_CDC 1   // board has the USB CDC device stack (UsbCdcLink available)
#endif

enum class LinkId : uint8_t {
    CtrlUart = 0,
    Usb = 1,
    AuxUart = 2,
};
constexpr size_t kLinkCount = 3;

class Link {
public:
    Link(LinkId id, const char* name, uint8_t* rx_ring, size_t rx_ring_size) noexcept;
    virtual ~Link() = default;
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    LinkId id() const noexcept { return id_; }
    const char* name() const noexcept { return name_; }

    /** @brief True when the link can carry data (USB: configured by the host) */
    virtual bool connected() const noexcept { return true; }

    /**
     * @brief Send a buffer; blocks until it has left the device or timeout_ms passed.
     *        The buffer must stay valid until the call returns. Thread safe.
     */
    std::expected<void, ControllerError> send(const uint8_t* data, size_t len, uint32_t timeout_ms) noexcept;

    /**
     * @brief Read received bytes; waits up to timeout_ms for at least one byte.
     * @return number of bytes copied (0 on timeout). One reader thread only.
     */
    size_t read(uint8_t* out, size_t max, uint32_t timeout_ms) noexcept;

    uint32_t rx_overflows() const noexcept { return rx_overflows_; }
    uint32_t tx_errors() const noexcept { return tx_errors_; }

protected:
    /** @brief Start transmitting; completion is reported with tx_done_isr(). @return false if not started */
    virtual bool start_tx(const uint8_t* data, size_t len) noexcept = 0;
    /** @brief Abandon a transmission that did not complete in time */
    virtual void abort_tx() noexcept {}
    /** @brief Largest buffer start_tx() accepts in one go */
    virtual size_t max_tx_chunk() const noexcept { return 0xFFFF; }
    /** @brief Hook after a chunk was sent (USB: zero-length packet) */
    virtual bool after_chunk(size_t /*len*/, uint32_t /*timeout_ms*/) noexcept { return true; }

    void tx_done_isr(bool ok) noexcept;
    void rx_push_isr(const uint8_t* data, size_t len) noexcept;
    bool wait_tx(uint32_t timeout_ms) noexcept;

    os::mutex tx_mutex_;

private:
    LinkId id_;
    const char* name_;
    uint8_t* rx_ring_;
    size_t rx_size_;
    volatile size_t head_ = 0;   // written by the ISR
    volatile size_t tail_ = 0;   // read side (read by the ISR for overflow checks)
    volatile uint32_t rx_overflows_ = 0;
    uint32_t tx_errors_ = 0;
    volatile bool tx_ok_ = false;
    os::semaphore rx_sem_;
    os::semaphore tx_sem_;
};

/**
 * @brief UART link. RX uses ReceiveToIdle with DMA when the handle has an RX DMA
 *        (circular preferred), interrupts otherwise; TX uses DMA when available.
 *        Reception is restarted after line errors (no flow control on the board).
 */
class UartLink final : public Link {
public:
    UartLink(LinkId id, const char* name, UART_HandleTypeDef* huart, uint8_t* rx_ring, size_t rx_ring_size) noexcept;

    /** @brief Start reception (call once, after the RTOS is running) */
    std::expected<void, ControllerError> start() noexcept;

    /** @brief Change the baud rate at run time; @return the rate actually set */
    std::expected<uint32_t, ControllerError> set_baudrate(uint32_t baudrate) noexcept;
    uint32_t baudrate() const noexcept;

    UART_HandleTypeDef* handle() const noexcept { return huart_; }

    // HAL callback routing
    static UartLink* find(const UART_HandleTypeDef* huart) noexcept;
    void on_rx_event_isr(uint16_t size) noexcept;
    void on_tx_complete_isr() noexcept;
    void on_error_isr() noexcept;

protected:
    bool start_tx(const uint8_t* data, size_t len) noexcept override;
    void abort_tx() noexcept override;

private:
    static constexpr size_t kStagingSize = 256;

    UART_HandleTypeDef* huart_;
    uint8_t staging_[kStagingSize];   // ReceiveToIdle target; copied into the ring
    size_t staging_pos_ = 0;
    bool rx_circular_ = false;
    bool started_ = false;

    bool arm_rx() noexcept;
    uint32_t kernel_clock_hz() const noexcept;
};

/**
 * @brief USB CDC (virtual COM port) link. Needs the hooks comm_usb_rx_isr() and
 *        comm_usb_tx_done_isr() called from usbd_cdc_if.c (CDC_Receive_FS,
 *        CDC_TransmitCplt_FS).
 */
class UsbCdcLink final : public Link {
public:
    UsbCdcLink(uint8_t* rx_ring, size_t rx_ring_size) noexcept;

    bool connected() const noexcept override;

    static UsbCdcLink* instance() noexcept { return instance_; }
    void on_rx_isr(const uint8_t* data, size_t len) noexcept { rx_push_isr(data, len); }
    void on_tx_complete_isr() noexcept { tx_done_isr(true); }

protected:
    bool start_tx(const uint8_t* data, size_t len) noexcept override;
    void abort_tx() noexcept override;
    bool after_chunk(size_t len, uint32_t timeout_ms) noexcept override;

private:
    static UsbCdcLink* instance_;
};

#endif

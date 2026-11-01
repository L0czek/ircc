#include "link.hpp"
#include "log.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

// =============================================================================
// Link (ring buffer + blocking TX)
// =============================================================================

Link::Link(LinkId id, const char* name, uint8_t* rx_ring, size_t rx_ring_size) noexcept
    : tx_mutex_(nullptr),
      id_(id),
      name_(name),
      rx_ring_(rx_ring),
      rx_size_(rx_ring_size),
      rx_sem_(1, 0, nullptr),
      tx_sem_(1, 0, nullptr)
{
}

void Link::rx_push_isr(const uint8_t* data, size_t len) noexcept {
    size_t head = head_;
    for (size_t i = 0; i < len; ++i) {
        const size_t next = (head + 1) % rx_size_;
        if (next == tail_) {
            rx_overflows_ = rx_overflows_ + 1;   // reader too slow: drop (the frame CRC catches it)
            break;
        }
        rx_ring_[head] = data[i];
        head = next;
    }
    head_ = head;
    rx_sem_.release();
}

size_t Link::read(uint8_t* out, size_t max, uint32_t timeout_ms) noexcept {
    if (head_ == tail_) {
        if (!rx_sem_.try_acquire_for(timeout_ms) && head_ == tail_) {
            return 0;
        }
    }
    const size_t head = head_;
    size_t n = 0;
    while (tail_ != head && n < max) {
        out[n++] = rx_ring_[tail_];
        tail_ = (tail_ + 1) % rx_size_;
    }
    return n;
}

void Link::tx_done_isr(bool ok) noexcept {
    tx_ok_ = ok;
    tx_sem_.release();
}

bool Link::wait_tx(uint32_t timeout_ms) noexcept {
    return tx_sem_.try_acquire_for(timeout_ms) && tx_ok_;
}

std::expected<void, ControllerError> Link::send(const uint8_t* data, size_t len, uint32_t timeout_ms) noexcept {
    std::lock_guard lock(tx_mutex_);
    if (!connected()) {
        return std::unexpected(ControllerError::HAL_ERROR);
    }
    (void)tx_sem_.try_acquire();   // drop a stale completion
    while (len > 0) {
        const size_t chunk = std::min(len, max_tx_chunk());
        if (!start_tx(data, chunk)) {
            ++tx_errors_;
            return std::unexpected(ControllerError::HAL_BUSY);
        }
        if (!wait_tx(timeout_ms)) {
            abort_tx();
            ++tx_errors_;
            return std::unexpected(ControllerError::HAL_TIMEOUT);
        }
        if (!after_chunk(chunk, timeout_ms)) {
            ++tx_errors_;
            return std::unexpected(ControllerError::HAL_TIMEOUT);
        }
        data += chunk;
        len -= chunk;
    }
    return {};
}

// =============================================================================
// UartLink
// =============================================================================

namespace {
std::array<UartLink*, kLinkCount> uart_links{};
} // namespace

UartLink::UartLink(LinkId id, const char* name, UART_HandleTypeDef* huart, uint8_t* rx_ring,
                   size_t rx_ring_size) noexcept
    : Link(id, name, rx_ring, rx_ring_size), huart_(huart)
{
    for (auto& slot : uart_links) {
        if (!slot) {
            slot = this;
            break;
        }
    }
}

UartLink* UartLink::find(const UART_HandleTypeDef* huart) noexcept {
    for (UartLink* link : uart_links) {
        if (link && link->huart_ == huart) {
            return link;
        }
    }
    return nullptr;
}

bool UartLink::arm_rx() noexcept {
    staging_pos_ = 0;
    HAL_StatusTypeDef st;
    if (huart_->hdmarx) {
        rx_circular_ = huart_->hdmarx->Init.Mode == DMA_CIRCULAR;
        st = HAL_UARTEx_ReceiveToIdle_DMA(huart_, staging_, kStagingSize);
        // Half-transfer events only add interrupt load; idle/TC events are enough.
        __HAL_DMA_DISABLE_IT(huart_->hdmarx, DMA_IT_HT);
    } else {
        rx_circular_ = false;
        st = HAL_UARTEx_ReceiveToIdle_IT(huart_, staging_, kStagingSize);
    }
    return st == HAL_OK;
}

namespace {

// USART global interrupt of a UART instance (-1: unknown instance)
int uart_irq(const USART_TypeDef* u) {
    if (u == USART1) return USART1_IRQn;
    if (u == USART2) return USART2_IRQn;
#if defined(USART3)
    if (u == USART3) return USART3_IRQn;
#endif
#if defined(UART4)
    if (u == UART4) return UART4_IRQn;
#endif
#if defined(UART5)
    if (u == UART5) return UART5_IRQn;
#endif
    return -1;
}

} // namespace

std::expected<void, ControllerError> UartLink::start() noexcept {
    if (!huart_) {
        return std::unexpected(ControllerError::HAL_ERROR);
    }
    // ReceiveToIdle (IT and DMA) and TX completion need the USART global interrupt. It is
    // configured in CubeMX (which also generates the handler), so only report it here.
    if (const int irq = uart_irq(huart_->Instance); irq >= 0 && !NVIC_GetEnableIRQ(static_cast<IRQn_Type>(irq))) {
        os::error("%s: USART global interrupt disabled in CubeMX: link cannot receive\n", name());
    }
    (void)HAL_UART_AbortReceive(huart_);
    if (!arm_rx()) {
        os::error("%s: cannot start reception\n", name());
        return std::unexpected(ControllerError::HAL_ERROR);
    }
    started_ = true;
    return {};
}

void UartLink::on_rx_event_isr(uint16_t size) noexcept {
    // `size` is the fill position inside staging_ (cumulative for this reception).
    if (size > staging_pos_) {
        rx_push_isr(staging_ + staging_pos_, size - staging_pos_);
    }
    staging_pos_ = (size >= kStagingSize) ? 0 : size;
    // Non-circular reception ends at idle / buffer full: re-arm immediately.
    if (!rx_circular_ && huart_->RxState == HAL_UART_STATE_READY) {
        (void)arm_rx();
    }
}

void UartLink::on_error_isr() noexcept {
    // Overrun / framing / noise: HAL aborted the reception. Restart it; corrupted bytes are
    // rejected by the frame CRC.
    if (started_ && huart_->RxState == HAL_UART_STATE_READY) {
        (void)arm_rx();
    }
    if (huart_->gState == HAL_UART_STATE_READY) {
        tx_done_isr(false);   // a transmission aborted by the error never completes
    }
}

bool UartLink::start_tx(const uint8_t* data, size_t len) noexcept {
    auto* buf = const_cast<uint8_t*>(data);
    const HAL_StatusTypeDef st = huart_->hdmatx
                                     ? HAL_UART_Transmit_DMA(huart_, buf, static_cast<uint16_t>(len))
                                     : HAL_UART_Transmit_IT(huart_, buf, static_cast<uint16_t>(len));
    return st == HAL_OK;
}

void UartLink::on_tx_complete_isr() noexcept {
    tx_done_isr(true);
}

void UartLink::abort_tx() noexcept {
    (void)HAL_UART_AbortTransmit(huart_);
}

uint32_t UartLink::kernel_clock_hz() const noexcept {
    const USART_TypeDef* u = huart_->Instance;
    if (u == USART1) return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_USART1);
    if (u == USART2) return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_USART2);
#if defined(USART3)
    if (u == USART3) return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_USART3);
#endif
#if defined(UART4)
    if (u == UART4) return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_UART4);
#endif
#if defined(UART5)
    if (u == UART5) return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_UART5);
#endif
    return 0;
}

uint32_t UartLink::baudrate() const noexcept {
    const uint32_t clk = kernel_clock_hz();
    const uint32_t brr = huart_->Instance->BRR;
    if (clk == 0 || brr == 0) {
        return 0;
    }
    if (huart_->Instance->CR1 & USART_CR1_OVER8) {
        const uint32_t div = (brr & 0xFFF0u) | ((brr & 0x7u) << 1);
        return div ? (2u * clk + div / 2u) / div : 0;
    }
    return (clk + brr / 2u) / brr;
}

std::expected<uint32_t, ControllerError> UartLink::set_baudrate(uint32_t baudrate) noexcept {
    std::lock_guard lock(tx_mutex_);   // no transmission in progress while reconfiguring
    const uint32_t clk = kernel_clock_hz();
    if (baudrate == 0 || clk == 0 || baudrate > clk / 8u || baudrate < clk / (16u * 0xFFFFu) + 1u) {
        return std::unexpected(ControllerError::HAL_ERROR);
    }
    (void)HAL_UART_Abort(huart_);
    huart_->Init.BaudRate = baudrate;
    huart_->Init.OverSampling = (baudrate > clk / 16u) ? UART_OVERSAMPLING_8 : UART_OVERSAMPLING_16;
    if (HAL_UART_Init(huart_) != HAL_OK) {
        return std::unexpected(ControllerError::HAL_ERROR);
    }
    if (started_ && !arm_rx()) {
        return std::unexpected(ControllerError::HAL_ERROR);
    }
    return this->baudrate();
}

// =============================================================================
// HAL UART callbacks -> links
// =============================================================================

extern "C" void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t size) {
    if (UartLink* link = UartLink::find(huart)) {
        link->on_rx_event_isr(size);
    }
}

extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart) {
    if (UartLink* link = UartLink::find(huart)) {
        link->on_tx_complete_isr();
    }
}

extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart) {
    if (UartLink* link = UartLink::find(huart)) {
        link->on_error_isr();
    }
}

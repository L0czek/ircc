#include "link.hpp"

#if defined(COMM_HAS_USB_CDC)
#include "usb_device.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

extern USBD_HandleTypeDef hUsbDeviceFS;

UsbCdcLink* UsbCdcLink::instance_ = nullptr;

UsbCdcLink::UsbCdcLink(uint8_t* rx_ring, size_t rx_ring_size) noexcept
    : Link(LinkId::Usb, "usb", rx_ring, rx_ring_size)
{
    instance_ = this;
}

bool UsbCdcLink::connected() const noexcept {
    return hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED && hUsbDeviceFS.pClassData != nullptr;
}

bool UsbCdcLink::start_tx(const uint8_t* data, size_t len) noexcept {
    if (!connected()) {
        return false;
    }
    return CDC_Transmit_FS(const_cast<uint8_t*>(data), static_cast<uint16_t>(len)) == USBD_OK;
}

bool UsbCdcLink::after_chunk(size_t len, uint32_t timeout_ms) noexcept {
    // A bulk transfer whose length is a multiple of the packet size only ends for the
    // host when a short (here: zero-length) packet follows.
    if (len == 0 || (len % CDC_DATA_FS_MAX_PACKET_SIZE) != 0) {
        return true;
    }
    static uint8_t zlp;
    if (!start_tx(&zlp, 0)) {
        return false;
    }
    if (!wait_tx(timeout_ms)) {
        abort_tx();
        return false;
    }
    return true;
}

void UsbCdcLink::abort_tx() noexcept {
    // The host is not reading (port closed): drop the pending IN transfer so later
    // transmissions are not refused with USBD_BUSY forever.
    auto* hcdc = static_cast<USBD_CDC_HandleTypeDef*>(hUsbDeviceFS.pClassData);
    if (hcdc) {
        (void)USBD_LL_FlushEP(&hUsbDeviceFS, CDC_IN_EP);
        hcdc->TxState = 0;
    }
}

// Hooks called from usbd_cdc_if.c (USER CODE sections)
extern "C" void comm_usb_rx_isr(const uint8_t* buf, uint32_t len) {
    UsbCdcLink* link = UsbCdcLink::instance();
    if (link) {
        link->on_rx_isr(buf, len);
    }
}

extern "C" void comm_usb_tx_done_isr(void) {
    UsbCdcLink* link = UsbCdcLink::instance();
    if (link) {
        link->on_tx_complete_isr();
    }
}

#endif

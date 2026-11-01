#include <cstdio>
#include <expected>
#include <memory>

#include "ctl.h"
#include "global.hpp"

#include "message_handler.hpp"
#include "broker.hpp"
#include "link.hpp"
#include "scope_service.hpp"
#include "diagnostics.hpp"
#include "charger.hpp"
#include "output_voltage_regulator.hpp"
#include "aux_pwm_applet.hpp"
#include "cmsis_os2.h"
#include "config.hpp"
#include "log.hpp"
#include "thread.hpp"
#include "oscilloscope.hpp"
#include "board_io.hpp"
#include "power_path.hpp"
#include "usbpd_app.hpp"

MessageHandler message_handler;
static Diagnostics diagnostics;
Charger* charger = nullptr;
OutputVoltageRegulator* output_voltage_regulator = nullptr;

#if defined(resonant_converter_controller) && defined(OSCILLOSCOPE_SUPPORTED)
extern psram::PSRAMDriver* g_psram_driver;   // memtest.cc
Oscilloscope* oscilloscope = nullptr;
#endif


// Host links: requests on the ctrl UART and USB CDC are answered on the link they came
// from; the aux UART is a data-only link for bulk transfers (DataLinkConfigure sets its
// baud rate). See broker.hpp / scope_service.hpp and docs/host_protocol.md.
static void start_host_links() {
    static uint8_t ctrl_ring[1024];
    static UartLink ctrl_link(LinkId::CtrlUart, "ctrl-uart", BOARD_CONFIG.message_bus, ctrl_ring, sizeof(ctrl_ring));
    static Broker broker(message_handler);
    diagnostics.attach(broker);
    message_handler.set_diagnostics(&diagnostics);
    broker.add_link(ctrl_link, true);
    (void)ctrl_link.start();

#if defined(COMM_HAS_USB_CDC)
    static uint8_t usb_ring[2048];
    static UsbCdcLink usb_link(usb_ring, sizeof(usb_ring));
    broker.add_link(usb_link, true);
#endif

    UartLink* aux = nullptr;
#if defined(resonant_converter_controller)
    static uint8_t aux_ring[64];   // data-only link: received bytes are discarded
    static UartLink aux_link(LinkId::AuxUart, "aux-uart", g_board_aux_uart, aux_ring, sizeof(aux_ring));
    broker.add_link(aux_link, false);
    (void)aux_link.start();
    aux = &aux_link;
#endif

#if defined(resonant_converter_controller) && defined(OSCILLOSCOPE_SUPPORTED)
    static ScopeService scope_service(broker, oscilloscope, aux);
#else
    static ScopeService scope_service(broker, aux);
#endif
    message_handler.set_scope_service(&scope_service);
    scope_service.start();
    broker.start();
}

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
    Diagnostics::capture_reset_flags();
    diagnostics.start();   // keeps boot messages for the first host that subscribes
    os::info("Booting up\n");

    // Initialize charger with I2C handle from board config
    charger = new Charger(g_board_i2c_internal);
    if (!charger->initialize()) {
        os::error("Failed to initialize charger\n");
    }

    // Initialize output voltage regulator with I2C handle from board config
#if defined(resonant_converter_controller)
    // USB-C power policy (docs/usb_pd.md): input selection, IINDPM/VINDPM from the PD
    // contract or the dock limit, USB-C source via OTG. The stack is already running.
    static PowerPath power_path(*charger, usb_pd_port());
    power_path.start();
    message_handler.set_usb_pd(&usb_pd_port());
#endif

    output_voltage_regulator = new OutputVoltageRegulator(g_board_i2c_internal);
    if (!output_voltage_regulator->initialize()) {
        os::error("Failed to initialize output voltage regulator\n");
    }

#if defined(resonant_converter_controller) && defined(OSCILLOSCOPE_SUPPORTED)
    // PSRAM (2x APS6404L, QUADSPI dual-flash) and the long-trace oscilloscope on top of it.
    // A QUADSPI configuration that cannot meet the PSRAM timing is reported and leaves the
    // oscilloscope unusable (configure() returns PSRAM_NotInitialized).
    static psram::PSRAMDriver psram(g_board_psram_qspi);
    if (auto r = psram.psram_init(); !r) {
        char config[192];
        psram.psram_describe_config(config, sizeof(config));
        os::error("PSRAM init failed: %s [%s]\n", r.error().message().c_str(), config);
        Diagnostics::set_psram_state(false, config);
    } else {
        char config[160];
        psram.psram_describe_config(config, sizeof(config));
        Diagnostics::set_psram_state(true, config);
    }
    g_psram_driver = &psram;
    static Oscilloscope scope(psram, g_board_tim_adc_trgo);
    oscilloscope = &scope;
#endif

    // Note: applets are started on-demand via applet_command RPC command
    // aux-pwm can be started with: applet_command { applet_name: "aux-pwm", args: ["start"] }

    start_host_links();

    // Encoder (TIM20) and WS2812B LED (TIM5); the fan is started by its owner
    board::start_io();

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

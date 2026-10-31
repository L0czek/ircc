#include "i2c_bus.hpp"
#include "board.h"
#include "cmsis_os2.h"
#include "log.hpp"
#include "stm32g4xx_hal.h"

namespace {

struct BusPin {
    GPIO_TypeDef* port;
    uint16_t pin;
};

// Pins of the I2C peripheral behind g_board_i2c_internal (see the board's .ioc).
#if defined(resonant_converter_controller)
// I2C1: SCL = PB8 (BOOT0), SDA = PB9
BusPin scl_pin() { return {GPIOB, GPIO_PIN_8}; }
BusPin sda_pin() { return {GPIOB, GPIO_PIN_9}; }
#elif defined(stm32g474re_nucleo_board)
// I2C3: SCL = PC8, SDA = PC9
BusPin scl_pin() { return {GPIOC, GPIO_PIN_8}; }
BusPin sda_pin() { return {GPIOC, GPIO_PIN_9}; }
#else
#error "i2c_bus.cc: define the internal I2C bus pins for this board"
#endif

uint32_t recovery_count = 0;

// Busy wait; only used during recovery with the bus lock held.
void delay_us(uint32_t us) {
    const uint32_t loops = (SystemCoreClock / 1'000'000u) * us / 4u;  // >= ~4 cycles per iteration
    for (uint32_t i = 0; i < loops; ++i) {
        __NOP();  // asm volatile: keeps the loop from being optimised away
    }
}

bool is_high(const BusPin& p) {
    return HAL_GPIO_ReadPin(p.port, p.pin) == GPIO_PIN_SET;
}

void drive(const BusPin& p, bool high) {
    HAL_GPIO_WritePin(p.port, p.pin, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

// Release SCL and wait for a slave that stretches the clock (bounded).
void release_scl(const BusPin& scl) {
    drive(scl, true);
    for (uint32_t t = 0; t < 1000 && !is_high(scl); ++t) {
        delay_us(1);
    }
}

} // namespace

os::mutex& i2c_internal_bus_mutex() {
    // Priority inheritance: the bus is shared between threads of different priority.
    static osMutexAttr_t attrs = {
        .name = "i2c-internal",
        .attr_bits = osMutexPrioInherit,
        .cb_mem = nullptr,
        .cb_size = 0,
    };
    // First call happens in controller_system_init() before the worker threads exist,
    // so the (non thread-safe, -fno-threadsafe-statics) initialisation is race free.
    static os::mutex bus(&attrs);
    return bus;
}

uint32_t i2c_internal_bus_recovery_count() noexcept {
    return recovery_count;
}

bool i2c_internal_bus_recover() noexcept {
    I2C_HandleTypeDef* hi2c = g_board_i2c_internal;
    if (!hi2c || !hi2c->Instance) {
        return false;
    }
    ++recovery_count;

    // HAL_I2C_Init() rewrites CR1, which also holds the filter settings applied
    // by MX_I2Cx_Init(); remember them to restore after re-initialisation.
    const uint32_t cr1 = hi2c->Instance->CR1;
    const uint32_t analog_filter = (cr1 & I2C_CR1_ANFOFF) ? I2C_ANALOGFILTER_DISABLE : I2C_ANALOGFILTER_ENABLE;
    const uint32_t digital_filter = (cr1 & I2C_CR1_DNF) >> I2C_CR1_DNF_Pos;

    (void)HAL_I2C_DeInit(hi2c);  // MSP de-init releases the pins

    const BusPin scl = scl_pin();
    const BusPin sda = sda_pin();
    drive(scl, true);
    drive(sda, true);
    GPIO_InitTypeDef gpio = {};
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_NOPULL;  // external bus pull-ups
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Pin = scl.pin;
    HAL_GPIO_Init(scl.port, &gpio);
    gpio.Pin = sda.pin;
    HAL_GPIO_Init(sda.port, &gpio);
    delay_us(5);

    // A slave interrupted mid-byte keeps driving SDA low until it has clocked
    // out the rest of the byte: give it up to 9 clocks (~100 kHz).
    for (int i = 0; i < 9 && !is_high(sda); ++i) {
        drive(scl, false);
        delay_us(5);
        release_scl(scl);
        delay_us(5);
    }

    // STOP condition: SDA rising while SCL is high.
    drive(scl, false);
    delay_us(5);
    drive(sda, false);
    delay_us(5);
    release_scl(scl);
    delay_us(5);
    drive(sda, true);
    delay_us(5);

    const bool released = is_high(sda) && is_high(scl);

    HAL_GPIO_DeInit(scl.port, scl.pin);
    HAL_GPIO_DeInit(sda.port, sda.pin);

    bool ok = HAL_I2C_Init(hi2c) == HAL_OK;  // MSP init restores the alternate function
    ok = ok && HAL_I2CEx_ConfigAnalogFilter(hi2c, analog_filter) == HAL_OK;
    ok = ok && HAL_I2CEx_ConfigDigitalFilter(hi2c, digital_filter) == HAL_OK;

    if (!released || !ok) {
        os::error("I2C bus recovery failed (SDA/SCL %s, re-init %s)\n",
                  released ? "released" : "still low", ok ? "ok" : "failed");
        return false;
    }
    os::warn("I2C bus recovered (%lu recoveries since boot)\n", static_cast<unsigned long>(recovery_count));
    return true;
}

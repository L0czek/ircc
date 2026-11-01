# CubeMX requirements: oscilloscope, PSRAM, controller runtime

What `target/resonant_converter_controller` must provide for the long-trace
oscilloscope (`controller/src/oscilloscope.cc`), the PSRAM driver
(`external/APS6404L_STM32_DRIVER`) and the controller threads. The firmware
validates most of this at run time and logs the reason when a setting is wrong:
`configure()` / `arm()` / `psram_init()` refuse instead of capturing garbage.

## How the data flows (why the settings matter)

```
TIM8 TRGO (update) ──► injected conversion on every ADC used (JEXTSEL = T8_TRGO)
TIM8 CCn, placed just after that ADC's conversion ends
      ──► DMA request TIM8_CHn ──► DMA copies ADCx JDRk ──► SRAM ring n (circular)
SRAM ring half/full interrupts ──► PSRAM writer: QUADSPI indirect-write bursts,
      data fed by the QUADSPI DMA channel, next burst started from its TC interrupt
```

- Injected conversions have no ADC DMA request (RM0440 21.4.26: DMA serves regular
  conversions only). The JDR copy is therefore paced by TIM8 compare channels.
- A DMAMUX request line can drive only one DMA channel (RM0440 13.4.3), so every
  oscilloscope channel needs its own TIM8 compare channel: **max 4 channels**
  (TIM8 CH1..CH4). Channel n of `ScopeConfig::channels` uses TIM8 CH(n+1).
- The ADC regular group stays free for other code; injected conversions preempt it.

## Checklist

### TIM8 (sample clock, `g_board_tim_adc_trgo`)
- [ ] Trigger output (TRGO): **Update event** (already set).
- [ ] Channel 1..N (N = number of scope channels): **Output Compare No Output**.
      Mode, pulse and prescaler/period are set by the firmware at run time.
- [ ] DMA requests **TIM8_CH1..TIM8_CHN**, one per channel:
  - Direction: Peripheral to Memory
  - Mode: **Circular**
  - Peripheral increment: off, Memory increment: on
  - Data width: Peripheral **Word** (or Half Word), Memory **Half Word**
  - Priority: High
- [ ] The DMA channel interrupts enabled (CubeMX does this by default).

### ADCs used by the oscilloscope (ADC1..ADC5)
- [ ] Injected conversion external trigger: **Timer 8 Trigger Out event (T8_TRGO)**,
      rising edge (already set on ADC1-3). The channel list, ranks and sampling
      times are programmed by the firmware.
- [ ] Max 4 scope channels per ADC (injected sequence length).
- [ ] Multimode: independent (current setting).
- [ ] Remove the ADC1/ADC2/ADC3 DMA requests that were added for the scope: they serve
      the regular group only and are never fired by injected conversions. Keep them only
      if some other code uses regular-group DMA.
- [ ] For the analog watchdog trigger: **ADC global interrupt** enabled
      (`ADC1_2_IRQn`, `ADC3_IRQn`, ...).
- Sampling time vs rate: the ADC clock is HCLK/4 = 42.5 MHz. A channel needs
  sampling time + 12.5 ADC cycles; a sample rate too high for the chosen sampling
  times is rejected by `configure()` with a log message. 2.5-cycle sampling (2.8 Msps)
  needs a low-impedance source.

### QUADSPI (2x APS6404L, dual-flash)
- [ ] Dual-flash: enabled (already set).
- [ ] **Flash Size: 23** (16 MB total of both chips; FSIZE = log2(bytes) - 1).
      Currently 1 (4 bytes): every PSRAM access fails.
- [ ] **Clock Prescaler: 2** (170 / 3 = 56.7 MHz) or 3 (42.5 MHz).
      Currently 255 (0.66 MHz): too slow for the 8 us max CE# low time (tCEM).
      Must stay <= 84 MHz (prescaler >= 2 at 170 MHz).
- [ ] **Chip Select High Time: 2 cycles** at 56.7 MHz (>= 18 ns tCPH); 1 cycle is
      enough at 42.5 MHz.
- [ ] **Fifo Threshold: 4** (or more). Needed for 32-bit DMA writes: the DMA request
      must only fire when 4 FIFO bytes are free. Currently 1: `psram_dma_begin()`
      refuses with ConfigError.
- [ ] Sample shifting: none is fine for writes; consider half-cycle above ~50 MHz if
      reads show errors.
- [ ] QUADSPI DMA request (already added on DMA1 Channel 8):
  - Direction: Memory to Peripheral, Mode: Normal, Priority: **Very High**
  - Data width: any - the driver reprograms the channel to word/word per burst.
  - Preferably on **DMA2**, so the PSRAM stream does not share a DMA controller with
    the TIM8 sample DMAs.
- [ ] Its DMA channel interrupt enabled (CubeMX default). The QUADSPI global
      interrupt is not needed.

### Comparator trigger (optional)
- [ ] The COMP used as trigger: output to EXTI, **Interrupt mode, rising edge**, and
      started by the application (`HAL_COMP_Start`). Pass its handle in
      `ScopeTriggerConfig::comparator`.
- HRTIM fault/event triggers: call `oscilloscope->trigger_from_isr(ScopeTriggerSource::External)`
  from the HRTIM callback and set `ScopeTriggerConfig::external = true`.

### NVIC priorities
- [ ] All oscilloscope DMA channels (TIM8_CHx and QUADSPI) at the **same priority**:
      their handlers share state without locking and must not preempt each other.
      (Today all DMA1 channels are at 5, which satisfies this.)
- [ ] ADC global and COMP EXTI interrupts (trigger sources) at the same or a lower
      priority than those DMA channels.
- [ ] Keep the converter control interrupts above all of these.

### FreeRTOS
- [ ] **TOTAL_HEAP_SIZE: at least 12288** (currently 3072). Allocated from it at boot:

  | Allocation                                   | Approx. bytes |
  |----------------------------------------------|---------------|
  | defaultTask (512 B stack + TCB)              | 600           |
  | USBPD CAD thread (1200 B) + PE thread (1400 B) + 3 queues | 3000 |
  | message handler sender + receiver threads    | 1200          |
  | power-service thread (1024 B stack)          | 1100          |
  | mutexes and semaphores (~8)                  | 650           |
  | **Total**                                    | **~6600**     |

  With 3072 bytes the USB-PD threads alone do not fit; 12288 leaves room for growth.

## RAM budget (resonant_converter_controller, 128 KB)

| Item                                      | Bytes   |
|-------------------------------------------|---------|
| .data + .bss today (incl. 3 KB FreeRTOS heap) | ~21 300 |
| Oscilloscope SRAM rings, 4 x 8 KB (linked once `arm()` is called) | 32 768 |
| FreeRTOS heap increase 3 KB -> 12 KB      | 9 216   |
| Linker minimum heap + main stack          | 1 536   |
| **Total**                                 | **~65 000** |

About 63 KB stays free for the newlib heap (`new`, `std::string`). The rings can be
shrunk via `Oscilloscope::kRingSamples` (half-ring time at 2.8 Msps: 0.72 ms with
4096 samples), or the scope limited to fewer channels.

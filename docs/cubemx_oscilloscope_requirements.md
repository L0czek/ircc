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

### Host links (USART1 control, USART2 aux, USB CDC) — see docs/host_protocol.md
- [ ] **USART1 and USART2 global interrupts: enable them (NVIC).** They are disabled
      today, and no `USARTx_IRQHandler` is generated, so neither UART link can receive
      (ReceiveToIdle needs the USART interrupt even with DMA). `UartLink::start()` logs
      `USART global interrupt disabled in CubeMX` until this is fixed.
- [ ] USART1 (control): RX DMA, **circular**, byte width (the HT interrupt is disabled at
      run time), and TX DMA, normal mode. Without DMA the link falls back to interrupt
      mode, which is fine at 115200 Bd.
- [ ] USART2 (aux data link): **TX DMA**, normal mode, byte width. DATA frames are about
      2 KB each, and at MBd rates per-byte TX interrupts would cost about 10 % CPU per MBd.
      RX DMA is not needed (the aux link carries no requests).
- [ ] USART2 kernel clock: the aux baud rate goes up to f<sub>USART</sub>/8 (OVER8 is
      chosen at run time), so use PCLK1 or SYSCLK, not HSI, for high rates.
- [ ] Optional: USB `APP_RX_DATA_SIZE` 2048 → 512 and `APP_TX_DATA_SIZE` 2048 → 64
      (`usbd_cdc_if.h`). TX goes straight from the broker buffer (`CDC_Transmit_FS`), so
      `UserTxBufferFS` is unused, and RX only needs one 64-byte packet. This saves about
      3.5 KB.

### FreeRTOS
- [ ] **TOTAL_HEAP_SIZE: at least 20480** (currently 3072). Allocated from it at boot:

  | Allocation                                          | Approx. bytes |
  |-----------------------------------------------------|---------------|
  | defaultTask (stack + TCB)                           | 2 100 (with 2 KB stack, see below) |
  | USBPD CAD thread (1200 B) + PE thread (1400 B) + 3 queues | 3 000   |
  | link RX threads, 2 × 1024 B (control UART, USB)     | 2 250         |
  | message-handler thread (3072 B)                     | 3 150         |
  | scope data pump thread (1536 B)                     | 1 600         |
  | power-service thread (1024 B)                       | 1 100         |
  | broker/scope queues, link and bus mutexes (~12)     | 1 200         |
  | **Total**                                           | **~14 400**   |

  20480 leaves about 6 KB for applets and growth.
- [ ] **defaultTask stack: at least 2048 bytes** (currently 128 words = 512 B). It runs
      `controller_system_init()`, which constructs the drivers, the broker and the scope
      service and logs with formatted output.

## RAM budget (resonant_converter_controller, 128 KB)

Measured on the current build (`arm-none-eabi-size`: data 836, bss 72 008):

| Item                                                     | Bytes   |
|----------------------------------------------------------|---------|
| .data + .bss today, of which:                            | 72 844  |
| &nbsp;&nbsp;oscilloscope SRAM rings, 4 × 8 KB            | 32 768  |
| &nbsp;&nbsp;broker request slots (2 × 2 KB) + TX frame buffer | 6 160 |
| &nbsp;&nbsp;in-place RX frame parsers (2 × 2 KB)         | 4 160   |
| &nbsp;&nbsp;scope DATA frame buffer                      | 2 064   |
| &nbsp;&nbsp;USB CDC RX/TX buffers                        | 4 096   |
| &nbsp;&nbsp;link RX rings (1 KB control, 2 KB USB, 64 B aux) | 3 136 |
| &nbsp;&nbsp;FreeRTOS heap (current 3 KB)                 | 3 072   |
| FreeRTOS heap increase 3 KB → 20 KB                      | 17 408  |
| Linker minimum heap + main stack                         | 1 536   |
| **Total**                                                | **~91 800** |

That leaves about 36 KB for the newlib heap (`new`: thread objects, links, drivers) and
growth; shrinking the USB buffers adds 3.5 KB. If RAM gets tight, the rings can be shrunk
via `Oscilloscope::kRingSamples` (half-ring time at 2.8 Msps: 0.72 ms with 4096
samples), or the scope limited to fewer channels.

//
// Created by andrew on 19/05/22.
//

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "uart_rx.pio.h"
#include "uart_tx.pio.h"

// for USB MIDI interface
#include "tusb.h"
#include "ump_device.h"
#include "pio_usb.h"
#include "usb_host_midi.h"

#define MIDI1_BAUD_RATE 31250

#ifndef HOST_PIN_DP
#define HOST_PIN_DP 20
#endif

#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN PICO_DEFAULT_LED_PIN
#endif

PIO pio = pio0;
uint smRx = 0;
uint smTx = 1;

void pio_rx_init(PIO piorx, uint smrx) {
    // Set up the state machine we're going to use to receive them.
    uint offset = pio_add_program(piorx, &uart_rx_program);
    uart_rx_program_init(piorx, smrx, offset, 13, MIDI1_BAUD_RATE);
}
void pio_tx_init(PIO piotx, uint smtx) {
    uint offset = pio_add_program(piotx, &uart_tx_program);
    uart_tx_program_init(piotx, smtx, offset, 12, MIDI1_BAUD_RATE);
}

// Phase 1: Device + Host デュアルロール初期化
// DIN MIDI は pio0、PIO USB Host は pio1 を使用（競合回避）
static void usb_dual_init(void) {
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = HOST_PIN_DP;
    pio_cfg.pio_tx_num = 1;
    pio_cfg.pio_rx_num = 1;
    pio_cfg.sm_tx = 0;
    pio_cfg.sm_rx = 1;
    pio_cfg.sm_eop = 2;

    tuh_configure(BOARD_HOST_RHPORT_NUM, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tusb_init();
}

//--------------------------------------------------------------------+
// Phase 6: Pico onboard LED — connection status
//
//   Solid ON     : PC UMP mounted AND ≥1 Host MIDI mounted (bridge ready)
//   Slow blink   : PC mounted, waiting for Host MIDI (~1 Hz)
//   Fast blink   : Host MIDI mounted, waiting for PC (~5 Hz)
//   Very slow    : neither (idle / not enumerated)
//--------------------------------------------------------------------+

static void status_led_init(void) {
    gpio_init(STATUS_LED_PIN);
    gpio_set_dir(STATUS_LED_PIN, GPIO_OUT);
    gpio_put(STATUS_LED_PIN, 0);
}

static void status_led_task(void) {
    static absolute_time_t next_toggle = {0};
    static bool led_on = false;

    const bool host = usb_host_midi_any_mounted();
    const bool pc   = tud_ump_n_mounted(0);

    if (host && pc) {
        gpio_put(STATUS_LED_PIN, 1);
        led_on = true;
        return;
    }

    uint32_t period_ms;
    if (host) {
        period_ms = 100;  // Host only
    } else if (pc) {
        period_ms = 500;  // PC only — waiting for Host
    } else {
        period_ms = 1000; // idle
    }

    if (!time_reached(next_toggle)) {
        return;
    }
    led_on = !led_on;
    gpio_put(STATUS_LED_PIN, led_on ? 1 : 0);
    next_toggle = make_timeout_time_ms(period_ms);
}

//--------------------------------------------------------------------+
// Phase 5: DIN helpers (UMP words over DIN UART, existing endian rules)
//--------------------------------------------------------------------+

static uint8_t din_midi_version(void) {
    // Match existing PC↔DIN endian: alt#0 → MIDI1 (BE), alt#1 → MIDI2 (LE).
    // When PC is absent, treat as MIDI2 / native UMP little-endian.
    if (!tud_ump_n_mounted(0)) {
        return 2;
    }
    return (uint8_t)(tud_alt_setting(0) + 1);
}

static void din_write_ump_word(uint32_t word) {
    if (din_midi_version() == 2) {
        // MIDI2 UMP: Little endian
        uart_tx_program_putc(pio, smTx, word & 0xff);
        uart_tx_program_putc(pio, smTx, (word >> 8) & 0xff);
        uart_tx_program_putc(pio, smTx, (word >> 16) & 0xff);
        uart_tx_program_putc(pio, smTx, (word >> 24) & 0xff);
    } else {
        // MIDI1UP: Big endian
        uart_tx_program_putc(pio, smTx, (word >> 24) & 0xff);
        uart_tx_program_putc(pio, smTx, (word >> 16) & 0xff);
        uart_tx_program_putc(pio, smTx, (word >> 8) & 0xff);
        uart_tx_program_putc(pio, smTx, word & 0xff);
    }
}

// Host MIDI IN → DIN OUT (registered with usb_host_midi)
static void host_ump_to_din(uint32_t ump) {
    din_write_ump_word(ump);
}

// PC UMP OUT → DIN + Host (single tud_ump_read fan-out)
static void bridge_pc_ump_to_din_and_host(void) {
    if (!tud_ump_n_mounted(0)) {
        return;
    }
    if (!tud_ump_n_available(0)) {
        return;
    }

    uint32_t UMPpacket[4];
    uint32_t umpCount = tud_ump_read(0, UMPpacket, 4);
    if (umpCount == 0) {
        return;
    }

    const uint8_t mMidiVersion = din_midi_version();

    for (uint32_t i = 0; i < umpCount; i++) {
        // PC → DIN
        din_write_ump_word(UMPpacket[i]);
        // PC → Host MIDI 1.0 (Group-routed)
        usb_host_midi_send_ump(UMPpacket[i]);
    }
    usb_host_midi_flush_tx();

    if (mMidiVersion == 2) {
        // Existing: echo back to USB Device in MIDI 2.0 mode
        tud_ump_write(0, UMPpacket, umpCount);
    }
}

// DIN IN → PC + Host
static void bridge_din_to_pc_and_host(void) {
    if (pio_sm_is_rx_fifo_empty(pio, smRx)) {
        return;
    }

    // Read one 32-bit UMP word from DIN (little-endian byte order on the wire)
    uint32_t ump = uart_rx_program_getc(pio, smRx);
    if (pio_sm_is_rx_fifo_empty(pio, smRx)) return;
    ump |= ((uint32_t)uart_rx_program_getc(pio, smRx)) << 8;
    if (pio_sm_is_rx_fifo_empty(pio, smRx)) return;
    ump |= ((uint32_t)uart_rx_program_getc(pio, smRx)) << 16;
    if (pio_sm_is_rx_fifo_empty(pio, smRx)) return;
    ump |= ((uint32_t)uart_rx_program_getc(pio, smRx)) << 24;

    // DIN → PC
    if (tud_ump_n_mounted(0) && tud_ump_n_writeable(0) >= 1) {
        tud_ump_write(0, &ump, 1);
    }

    // DIN → Host MIDI 1.0 (Group-routed)
    usb_host_midi_send_ump(ump);
    usb_host_midi_flush_tx();
}

int main() {
    // PIO USB 要件: 120 MHz。DIN UART の baud 計算より先に変更する
    set_sys_clock_khz(120000, true);

    stdio_init_all();
    status_led_init();

    //---------- Setup MIDI Din Ports（pio0）
    pio_rx_init(pio, smRx);
    pio_tx_init(pio, smTx);

    // TinyUSB Device (RHPort0) + PIO USB Host (RHPort1 / pio1)
    usb_dual_init();
    usb_host_midi_init();
    usb_host_midi_set_ump_forward(host_ump_to_din);

    printf("DIN_Bridge: USB Device (MIDI 2.0) + PIO USB Host ready (D+=GP%u)\r\n", HOST_PIN_DP);
    printf("Phase 6: multi-device Group map, SysEx-safe converters, Pico LED status\r\n");
    printf("LED: solid=PC+Host, slow blink=wait Host, fast blink=wait PC\r\n");

// ------- Loop Process Incoming Messages
    while (true) {
        // Execute USB Device / Host stacks
        tud_task();
        tuh_task();

        // Phase 4/5/6: Host MIDI → UMP → PC (+ Host → DIN via forward cb)
        usb_host_midi_task();

        // Phase 5: PC UMP → DIN + Host MIDI 1.0
        bridge_pc_ump_to_din_and_host();

        // Phase 5: DIN → PC + Host MIDI 1.0
        bridge_din_to_pc_and_host();

        // Phase 6: connection status on Pico onboard LED
        status_led_task();
    }
    return 0;
}

//
// Created by andrew on 19/05/22.
//

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
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

// Optional Host VBUS enable (FET / load switch). Harmless if unused as GPIO.
#ifndef HOST_PIN_VBUSEN
#define HOST_PIN_VBUSEN 22
#endif

#ifndef HOST_PIN_VBUSEN_STATE
#define HOST_PIN_VBUSEN_STATE 1
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

//--------------------------------------------------------------------+
// Core1: PIO USB Host (SOF timer must run on this core)
// Official Pico-PIO-USB dual-role pattern.
//--------------------------------------------------------------------+

static void core1_host_main(void) {
    // Wait until core0 has enabled VBUS and finished shared setup
    sleep_ms(200);

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = HOST_PIN_DP;
    pio_cfg.pio_tx_num = 1;
    pio_cfg.pio_rx_num = 1;
    pio_cfg.sm_tx = 0;
    pio_cfg.sm_rx = 1;
    pio_cfg.sm_eop = 2;

    // tuh_configure must be called before host init
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

    // Init Host on core1 so PIO-USB SOF IRQ/timer stays on core1
    bool ok = tuh_init(BOARD_TUH_RHPORT);
    usb_host_logf("Host core1: tuh_init(%u)=%s\r\n", BOARD_TUH_RHPORT, ok ? "ok" : "FAIL");

    while (true) {
        tuh_task();
    }
}

static void host_vbus_init(void) {
    // Must be enabled BEFORE Host stack starts (bus-powered devices)
    gpio_init(HOST_PIN_VBUSEN);
    gpio_set_dir(HOST_PIN_VBUSEN, GPIO_OUT);
    gpio_put(HOST_PIN_VBUSEN, HOST_PIN_VBUSEN_STATE);
    sleep_ms(100); // let VBUS settle
}

static void usb_device_init(void) {
    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
}

//--------------------------------------------------------------------+
// Phase 6: Pico onboard LED — connection status
//
//   Solid ON     : PC UMP mounted AND ≥1 Host MIDI mounted (bridge ready)
//   Medium blink : PC mounted, USB device attached but not MIDI (~250 ms)
//   Slow blink   : PC mounted, waiting for any Host device (~500 ms)
//   Fast blink   : Host MIDI mounted, waiting for PC (~100 ms)
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

    const bool midi = usb_host_midi_any_mounted();
    const bool usb  = usb_host_any_device_attached();
    const bool pc   = tud_ump_n_mounted(0);

    if (midi && pc) {
        gpio_put(STATUS_LED_PIN, 1);
        led_on = true;
        return;
    }

    uint32_t period_ms;
    if (midi) {
        period_ms = 100;  // Host MIDI only
    } else if (pc && usb) {
        period_ms = 250;  // PC + non-MIDI USB device
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

// Periodic Host diagnosis over CDC
static void host_diag_task(void) {
    static absolute_time_t next_log = {0};
    static bool last_connected = false;

    if (!time_reached(next_log)) {
        return;
    }
    next_log = make_timeout_time_ms(2000);

    const bool connected = usb_host_port_connected();
    const bool usb = usb_host_any_device_attached();
    const bool midi = usb_host_midi_any_mounted();

    if (connected != last_connected || !midi) {
        printf("Host diag: line=%s usb_dev=%u midi=%u (D+=GP%u VBUSEN=GP%u)\r\n",
               connected ? "SE1/J" : "SE0",
               (unsigned)usb,
               (unsigned)midi,
               HOST_PIN_DP,
               HOST_PIN_VBUSEN);
        last_connected = connected;
    }
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

    for (uint32_t i = 0; i < umpCount; i++) {
        // PC → DIN
        din_write_ump_word(UMPpacket[i]);
        // PC → Host MIDI 1.0 (Group-routed)
        usb_host_midi_send_ump(UMPpacket[i]);
    }
    usb_host_midi_flush_tx();
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
    // PIO USB: 240 MHz improves bit-bang Host reliability vs 120 MHz
    set_sys_clock_khz(240000, true);
    sleep_ms(10);

    status_led_init();

    //---------- Setup MIDI Din Ports（pio0）
    pio_rx_init(pio, smRx);
    pio_tx_init(pio, smTx);

    usb_host_midi_init();
    usb_host_midi_set_ump_forward(host_ump_to_din);

    // Power Host port BEFORE starting PIO USB Host on core1
    host_vbus_init();

    // Core1: PIO USB Host (SOF + tuh_task)
    multicore_reset_core1();
    multicore_launch_core1(core1_host_main);

    // Core0: native USB Device
    usb_device_init();
    stdio_init_all();

    // Wait for CDC; do NOT flush buffered Host logs until connected
    absolute_time_t until = make_timeout_time_ms(5000);
    while (!time_reached(until) && !stdio_usb_connected()) {
        tud_task();
        tight_loop_contents();
    }
    sleep_ms(100);

    printf("DIN_Bridge: Device@core0 + Host@core1 @%luMHz (D+=GP%u VBUSEN=GP%u)\r\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000UL),
           HOST_PIN_DP, HOST_PIN_VBUSEN);
    printf("LED: solid=PC+MIDI, 250ms=PC+nonMIDI USB, 500ms=wait Host, 100ms=wait PC\r\n");
    printf("--- buffered Host events ---\r\n");
    usb_host_log_flush();
    printf("--- end buffered ---\r\n");

// ------- Loop Process Incoming Messages
    while (true) {
        // Device stack only on core0 (Host runs on core1)
        tud_task();

        // Phase 4/5/6: Host MIDI → UMP → PC (+ Host → DIN via forward cb)
        usb_host_midi_task();

        // Phase 5: PC UMP → DIN + Host MIDI 1.0
        bridge_pc_ump_to_din_and_host();

        // Phase 5: DIN → PC + Host MIDI 1.0
        bridge_din_to_pc_and_host();

        // Phase 6: connection status on Pico onboard LED
        status_led_task();
        host_diag_task();
        usb_host_log_flush();
    }
    return 0;
}

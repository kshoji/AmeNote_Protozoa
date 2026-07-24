//
// Created by andrew on 19/05/22.
//

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "uart_rx.pio.h"
#include "uart_tx.pio.h"

// for USB MIDI interface
#include "tusb.h"
#include "ump_device.h"
#include "pio_usb.h"
#include "usb_host_midi.h"

#include "include/bytestreamToUMP.h"
#include "include/umpToBytestream.h"

#define MIDI1_BAUD_RATE 31250

#ifndef HOST_PIN_DP
#define HOST_PIN_DP 20
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

int main() {
    // PIO USB 要件: 120 MHz。DIN UART の baud 計算より先に変更する
    set_sys_clock_khz(120000, true);

    stdio_init_all();

    //---------- Setup MIDI Din Ports（pio0）
    pio_rx_init(pio, smRx);
    pio_tx_init(pio, smTx);

    // TinyUSB Device (RHPort0) + PIO USB Host (RHPort1 / pio1)
    usb_dual_init();
    usb_host_midi_init();

    printf("DIN_Bridge: USB Device (MIDI 2.0) + PIO USB Host ready (D+=GP%u)\r\n", HOST_PIN_DP);
    printf("Phase 4: Host MIDI 1.0 → UMP → PC\r\n");

// ------- Loop Process Incoming Messages
    while (true) {
        // Execute USB Device / Host stacks
        tud_task();
        tuh_task();

        // Phase 4: Host MIDI → UMP → PC (falls back to serial dump if PC absent)
        usb_host_midi_task();

        // Existing: PC ↔ DIN bridge
        if (tud_ump_n_mounted(0)) {
            uint32_t ump_n_available = tud_ump_n_available(0);
            uint32_t UMPpacket[4];
            uint32_t umpCount;
            if (ump_n_available) {
                uint8_t mMidiVersion = tud_alt_setting(0) + 1;
                if ((umpCount = tud_ump_read(0, UMPpacket, 4))) {

                    for (uint8_t i = 0; i < umpCount; i++) {
                        // Write UMP stream to DIN port
                        if (mMidiVersion == 2) {
                            // MIDI2 UMP: Little endian
                            uart_tx_program_putc(pio, smTx, UMPpacket[i] & 0xff);
                            uart_tx_program_putc(pio, smTx, (UMPpacket[i] >> 8) & 0xff);
                            uart_tx_program_putc(pio, smTx, (UMPpacket[i] >> 16) & 0xff);
                            uart_tx_program_putc(pio, smTx, (UMPpacket[i] >> 24) & 0xff);
                        } else {
                            // MIDI1UP: Big endian
                            uart_tx_program_putc(pio, smTx, (UMPpacket[i] >> 24) & 0xff);
                            uart_tx_program_putc(pio, smTx, (UMPpacket[i] >> 16) & 0xff);
                            uart_tx_program_putc(pio, smTx, (UMPpacket[i] >> 8) & 0xff);
                            uart_tx_program_putc(pio, smTx, UMPpacket[i] & 0xff);
                        }
                    }

                    if (mMidiVersion == 2) {
                        // echo back to usb serial
                        tud_ump_write(0, UMPpacket, umpCount);
                    }
                }
            }

            //-------------------
            if (!pio_sm_is_rx_fifo_empty(pio, smRx)) {
                // Read UMP stream from DIN Port
                uint32_t ump = uart_rx_program_getc(pio, smRx);
                if (pio_sm_is_rx_fifo_empty(pio, smRx)) continue;
                ump |= ((uint32_t)uart_rx_program_getc(pio, smRx)) << 8;
                if (pio_sm_is_rx_fifo_empty(pio, smRx)) continue;
                ump |= ((uint32_t)uart_rx_program_getc(pio, smRx)) << 16;
                if (pio_sm_is_rx_fifo_empty(pio, smRx)) continue;
                ump |= ((uint32_t)uart_rx_program_getc(pio, smRx)) << 24;
                tud_ump_write(0, &ump, 1);
            }
        }
    }
    return 0;
}

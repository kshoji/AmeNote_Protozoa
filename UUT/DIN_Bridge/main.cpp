//
// Created by andrew on 19/05/22.
//



#include "hardware/pio.h"
#include "uart_rx.pio.h"
#include "uart_tx.pio.h"

// for USB MIDI interface
#include "tusb.h"
#include "ump_device.h"

#include "include/bytestreamToUMP.h"
#include "include/umpToBytestream.h"

#define MIDI1_BAUD_RATE 31250

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

int main() {

    //---------- Setup MIDI Din Ports
    // Setup pio for receive
    pio_rx_init(pio, smRx);
    // Setup pio for transmit
    pio_tx_init(pio, smTx);

    // Setup for TinyUSB
    tusb_init();

// ------- Loop Process incoming Messages
    while (true) {
        // Execute USB
        tud_task();

        //Read USB MIDI
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


/*
 * USB MIDI 1.0 Host — mount / MIDI1↔UMP / PC·DIN bridge (Phase 2–6)
 */

#ifndef USB_HOST_MIDI_H_
#define USB_HOST_MIDI_H_

#include <stdbool.h>
#include <stdint.h>

#include "tusb_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool     mounted;
  uint8_t  daddr;
  uint8_t  bInterfaceNumber;
  uint8_t  rx_cable_count;
  uint8_t  tx_cable_count;
  uint8_t  group; /**< UMP Group (0–15), assigned as idx on mount */
  uint16_t vid;
  uint16_t pid;
} usb_host_midi_dev_t;

/** Optional sink for each Host→UMP word (e.g. DIN OUT). Called before PC write. */
typedef void (*usb_host_ump_forward_cb_t)(uint32_t ump);

void usb_host_midi_init(void);

bool usb_host_midi_is_mounted(uint8_t idx);

/** True if any Host MIDI interface is mounted. */
bool usb_host_midi_any_mounted(void);

/** True if any USB device is attached on the Host port (any class). */
bool usb_host_any_device_attached(void);

/**
 * PIO-USB root-port line state: true if not SE0 (device electrically present
 * or bus idle J/K). Useful before enumeration completes.
 */
bool usb_host_port_connected(void);

uint8_t usb_host_midi_mounted_count(void);

const usb_host_midi_dev_t *usb_host_midi_get(uint8_t idx);

/**
 * Find mounted Host MIDI device with TX cable for the given UMP group.
 * Falls back to first TX device if no exact group match.
 */
bool usb_host_midi_tx_idx_for_group(uint8_t group, uint8_t *idx_out);

/**
 * Find first mounted Host MIDI device that has at least one TX cable.
 * Returns false if none.
 */
bool usb_host_midi_first_tx_idx(uint8_t *idx_out);

/**
 * Host MIDI → UMP → PC (`tud_ump_write`) + optional forward (DIN).
 * If PC UMP interface is not mounted, drain/dump to CDC.
 */
void usb_host_midi_task(void);

/** Register observer for Host-converted UMP words (Host MIDI IN → DIN OUT). */
void usb_host_midi_set_ump_forward(usb_host_ump_forward_cb_t cb);

/**
 * Convert one UMP word to MIDI 1.0 and queue to the Host device for that
 * UMP Group (`tuh_midi_stream_write`). Call `usb_host_midi_flush_tx()` after a batch.
 */
void usb_host_midi_send_ump(uint32_t ump);

/** Flush queued Host MIDI OUT packets (`tuh_midi_write_flush`). */
void usb_host_midi_flush_tx(void);

/** Pop one converted UMP word from the host RX ring. */
bool usb_host_midi_pop_ump(uint32_t *ump);

uint32_t usb_host_midi_ump_available(void);

/** Buffer a Host diagnostic line (safe before CDC is up; flushed on core0). */
void usb_host_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** Print any buffered Host logs to CDC (call from core0). */
void usb_host_log_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_HOST_MIDI_H_ */

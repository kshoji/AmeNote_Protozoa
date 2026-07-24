/*
 * USB MIDI 1.0 Host — mount tracking + MIDI 1.0 → UMP → PC (Phase 2/3/4)
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
  uint16_t vid;
  uint16_t pid;
} usb_host_midi_dev_t;

void usb_host_midi_init(void);

bool usb_host_midi_is_mounted(uint8_t idx);

const usb_host_midi_dev_t *usb_host_midi_get(uint8_t idx);

/**
 * Phase 4: Host MIDI → UMP → PC (`tud_ump_write`).
 * If PC UMP interface is not mounted, drain/dump to CDC (Phase 3 debug).
 */
void usb_host_midi_task(void);

/** Pop one converted UMP word from the host RX ring. */
bool usb_host_midi_pop_ump(uint32_t *ump);

uint32_t usb_host_midi_ump_available(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_HOST_MIDI_H_ */

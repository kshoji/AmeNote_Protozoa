/*
 * USB MIDI 1.0 Host — recognition (Phase 2) + MIDI 1.0 → UMP (Phase 3)
 *                 + bridge to PC USB Device (Phase 4)
 *
 * TinyUSB API note: unmount callback is tuh_midi_umount_cb (not unmount).
 * USB MIDI packet byte0 = (cable << 4) | CIN  → CIN = byte0 & 0x0F.
 */

#include "usb_host_midi.h"

#include <stdio.h>
#include <string.h>

#include "tusb.h"
#include "ump_device.h"
#include "include/bytestreamToUMP.h"

#ifndef USB_HOST_UMP_RING_SIZE
#define USB_HOST_UMP_RING_SIZE 128
#endif

// Set to 1 to also printf each UMP word sent to PC (can affect timing)
#ifndef USB_HOST_MIDI_DEBUG_UMP
#define USB_HOST_MIDI_DEBUG_UMP 0
#endif

static usb_host_midi_dev_t s_host_midi[CFG_TUH_MIDI];
static bytestreamToUMP s_host2ump;

static uint32_t s_ump_ring[USB_HOST_UMP_RING_SIZE];
static volatile uint16_t s_ump_head = 0;
static volatile uint16_t s_ump_tail = 0;

//--------------------------------------------------------------------+
// UMP ring buffer
//--------------------------------------------------------------------+

static uint16_t ump_ring_count(void) {
  return (uint16_t)((s_ump_head - s_ump_tail + USB_HOST_UMP_RING_SIZE) % USB_HOST_UMP_RING_SIZE);
}

static bool ump_ring_push(uint32_t ump) {
  uint16_t next = (uint16_t)((s_ump_head + 1) % USB_HOST_UMP_RING_SIZE);
  if (next == s_ump_tail) {
    return false; // full
  }
  s_ump_ring[s_ump_head] = ump;
  s_ump_head = next;
  return true;
}

static bool ump_ring_pop(uint32_t *ump) {
  if (s_ump_head == s_ump_tail) {
    return false;
  }
  *ump = s_ump_ring[s_ump_tail];
  s_ump_tail = (uint16_t)((s_ump_tail + 1) % USB_HOST_UMP_RING_SIZE);
  return true;
}

static void ump_ring_clear(void) {
  s_ump_head = 0;
  s_ump_tail = 0;
}

static void flush_converter_to_ring(void) {
  while (s_host2ump.availableUMP()) {
    uint32_t ump = s_host2ump.readUMP();
    if (!ump_ring_push(ump)) {
      printf("MIDI Host UMP ring full, dropping 0x%08lX\r\n", (unsigned long)ump);
    }
  }
}

//--------------------------------------------------------------------+
// USB MIDI 1.0 packet → bytestream → UMP
//--------------------------------------------------------------------+

// USB MIDI 1.0 CIN → number of valid MIDI payload bytes in the 4-byte packet
static uint8_t cin_payload_len(uint8_t cin) {
  static const uint8_t k_lens[16] = {
      0, // 0x0 Misc (reserved)
      0, // 0x1 Cable events (reserved)
      2, // 0x2 SysCom 2-byte
      3, // 0x3 SysCom 3-byte
      3, // 0x4 SysEx start/continue
      1, // 0x5 SysEx end 1 / 1-byte syscom
      2, // 0x6 SysEx end 2
      3, // 0x7 SysEx end 3
      3, // 0x8 Note Off
      3, // 0x9 Note On
      3, // 0xA Poly Key Pressure
      3, // 0xB Control Change
      2, // 0xC Program Change
      2, // 0xD Channel Pressure
      3, // 0xE Pitch Bend
      1, // 0xF Single-byte
  };
  return k_lens[cin & 0x0F];
}

static void convert_usb_midi_packet(const uint8_t packet[4]) {
  const uint8_t cin = (uint8_t)(packet[0] & 0x0F);
  const uint8_t len = cin_payload_len(cin);
  if (len == 0) {
    return;
  }

  // Skip all-zero padding packets some devices append to fill wMaxPacketSize
  if (packet[1] == 0 && packet[2] == 0 && packet[3] == 0 && cin == 0) {
    return;
  }

  for (uint8_t i = 0; i < len; i++) {
    const uint8_t b = packet[1 + i];
    // Active Sensing — skip (Phase 3 / Phase 6)
    if (b == MIDI_STATUS_SYSREAL_ACTIVE_SENSING) {
      continue;
    }
    s_host2ump.bytestreamParse(b);
  }

  flush_converter_to_ring();
}

static void process_usb_host_midi_rx(uint8_t idx) {
  if (!tuh_midi_mounted(idx)) {
    return;
  }

  uint8_t packet[4];
  while (tuh_midi_packet_read(idx, packet)) {
    convert_usb_midi_packet(packet);
  }
}

//--------------------------------------------------------------------+
// Phase 4: Host UMP ring → PC USB Device
//--------------------------------------------------------------------+

static void bridge_host_to_ump_device(void) {
  if (!tud_ump_n_mounted(0)) {
    return;
  }

  while (ump_ring_count() > 0) {
    if (tud_ump_n_writeable(0) < 1) {
      break; // wait for TX FIFO space; keep words in ring
    }

    uint32_t ump;
    if (!ump_ring_pop(&ump)) {
      break;
    }

    uint16_t written = tud_ump_write(0, &ump, 1);
    if (written == 0) {
      // Should be rare after writeable check; drop rather than reorder.
      printf("MIDI Host → PC: tud_ump_write failed for 0x%08lX\r\n", (unsigned long)ump);
      break;
    }

#if USB_HOST_MIDI_DEBUG_UMP
    printf("Host MIDI → PC UMP: 0x%08lX (alt=%u)\r\n",
           (unsigned long)ump, (unsigned)tud_alt_setting(0));
#endif
  }
}

static void drain_ump_ring_to_serial(void) {
  uint32_t ump;
  while (ump_ring_pop(&ump)) {
    printf("Host MIDI → UMP: 0x%08lX (PC not mounted)\r\n", (unsigned long)ump);
  }
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void usb_host_midi_init(void) {
  memset(s_host_midi, 0, sizeof(s_host_midi));
  ump_ring_clear();
  s_host2ump.defaultGroup = 0;
  s_host2ump.outputMIDI2  = false; // MIDI 1.0 Channel Voice UMP (Type 0x2)
}

bool usb_host_midi_is_mounted(uint8_t idx) {
  if (idx >= CFG_TUH_MIDI) {
    return false;
  }
  return s_host_midi[idx].mounted;
}

const usb_host_midi_dev_t *usb_host_midi_get(uint8_t idx) {
  if (idx >= CFG_TUH_MIDI) {
    return NULL;
  }
  return &s_host_midi[idx];
}

bool usb_host_midi_pop_ump(uint32_t *ump) {
  if (ump == NULL) {
    return false;
  }
  return ump_ring_pop(ump);
}

uint32_t usb_host_midi_ump_available(void) {
  return ump_ring_count();
}

void usb_host_midi_task(void) {
  if (tud_ump_n_mounted(0)) {
    // Phase 4: deliver converted UMP to PC as MIDI 2.0 / MIDI 1.0 (via tusb_ump)
    bridge_host_to_ump_device();
  } else {
    // Phase 3 fallback: dump while PC is absent so the ring does not stall
    drain_ump_ring_to_serial();
  }
}

//--------------------------------------------------------------------+
// TinyUSB MIDI Host callbacks
//--------------------------------------------------------------------+

extern "C" void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *mount_cb_data) {
  if (idx >= CFG_TUH_MIDI || mount_cb_data == NULL) {
    return;
  }

  usb_host_midi_dev_t *dev = &s_host_midi[idx];
  memset(dev, 0, sizeof(*dev));
  dev->mounted          = true;
  dev->daddr            = mount_cb_data->daddr;
  dev->bInterfaceNumber = mount_cb_data->bInterfaceNumber;
  dev->rx_cable_count   = mount_cb_data->rx_cable_count;
  dev->tx_cable_count   = mount_cb_data->tx_cable_count;

  uint16_t vid = 0;
  uint16_t pid = 0;
  if (tuh_vid_pid_get(mount_cb_data->daddr, &vid, &pid)) {
    dev->vid = vid;
    dev->pid = pid;
  }

  printf("MIDI Host mount: idx=%u daddr=%u itf=%u VID=%04X PID=%04X rx_cables=%u tx_cables=%u\r\n",
         idx,
         dev->daddr,
         dev->bInterfaceNumber,
         dev->vid,
         dev->pid,
         dev->rx_cable_count,
         dev->tx_cable_count);
}

extern "C" void tuh_midi_umount_cb(uint8_t idx) {
  if (idx >= CFG_TUH_MIDI) {
    return;
  }

  usb_host_midi_dev_t *dev = &s_host_midi[idx];
  printf("MIDI Host umount: idx=%u daddr=%u VID=%04X PID=%04X\r\n",
         idx, dev->daddr, dev->vid, dev->pid);
  memset(dev, 0, sizeof(*dev));
  ump_ring_clear();
}

extern "C" void tuh_midi_rx_cb(uint8_t idx, uint32_t xferred_bytes) {
  (void)xferred_bytes;
  if (!usb_host_midi_is_mounted(idx)) {
    return;
  }
  process_usb_host_midi_rx(idx);
}

extern "C" void tuh_midi_tx_cb(uint8_t idx, uint32_t xferred_bytes) {
  (void)idx;
  (void)xferred_bytes;
}

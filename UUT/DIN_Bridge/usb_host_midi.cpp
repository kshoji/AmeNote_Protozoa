/*
 * USB MIDI 1.0 Host — Phase 2–6
 *
 * Phase 6:
 *  - Active Sensing (0xFE) filter (Host RX + reverse)
 *  - Per-idx bytestreamToUMP / umpToBytestream (SysEx + multi-device safe)
 *  - idx → UMP Group assignment; reverse route by Group
 *  - Drop that Group's UMP ring words + reset converters on umount
 *  - Larger ring + rate-limited drop logs (stress / long SysEx)
 *
 * TinyUSB API note: unmount callback is tuh_midi_umount_cb (not unmount).
 * USB MIDI packet byte0 = (cable << 4) | CIN  → CIN = byte0 & 0x0F.
 */

#include "usb_host_midi.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "tusb.h"
#include "ump_device.h"
#include "include/bytestreamToUMP.h"
#include "include/umpToBytestream.h"

#ifndef USB_HOST_UMP_RING_SIZE
#define USB_HOST_UMP_RING_SIZE 256
#endif

#ifndef USB_HOST_MIDI_DEBUG_UMP
#define USB_HOST_MIDI_DEBUG_UMP 0
#endif

#ifndef USB_HOST_MIDI_DEBUG_REVERSE
#define USB_HOST_MIDI_DEBUG_REVERSE 0
#endif

static usb_host_midi_dev_t s_host_midi[CFG_TUH_MIDI];
static bytestreamToUMP s_host2ump[CFG_TUH_MIDI];
static umpToBytestream s_device2host[CFG_TUH_MIDI];
static usb_host_ump_forward_cb_t s_ump_forward = NULL;

static uint32_t s_ump_ring[USB_HOST_UMP_RING_SIZE];
static volatile uint16_t s_ump_head = 0;
static volatile uint16_t s_ump_tail = 0;

static absolute_time_t s_next_drop_log;

//--------------------------------------------------------------------+
// UMP ring buffer
//--------------------------------------------------------------------+

static uint16_t ump_ring_count(void) {
  return (uint16_t)((s_ump_head - s_ump_tail + USB_HOST_UMP_RING_SIZE) % USB_HOST_UMP_RING_SIZE);
}

static bool ump_ring_push(uint32_t ump) {
  uint16_t next = (uint16_t)((s_ump_head + 1) % USB_HOST_UMP_RING_SIZE);
  if (next == s_ump_tail) {
    return false;
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

static uint8_t ump_word_group(uint32_t ump) {
  return (uint8_t)((ump >> 24) & 0x0F);
}

/** Remove pending Host→PC words that belong to a disconnected device's Group. */
static void ump_ring_drop_group(uint8_t group) {
  uint32_t keep[USB_HOST_UMP_RING_SIZE];
  uint16_t n = 0;
  uint32_t ump;
  while (ump_ring_pop(&ump)) {
    if (ump_word_group(ump) == group) {
      continue;
    }
    if (n < USB_HOST_UMP_RING_SIZE) {
      keep[n++] = ump;
    }
  }
  for (uint16_t i = 0; i < n; i++) {
    (void)ump_ring_push(keep[i]);
  }
}

static void log_ring_drop(uint32_t ump) {
  if (!time_reached(s_next_drop_log)) {
    return; // rate-limited
  }
  printf("MIDI Host UMP ring full, dropping 0x%08lX (count was %u)\r\n",
         (unsigned long)ump, (unsigned)USB_HOST_UMP_RING_SIZE - 1);
  s_next_drop_log = make_timeout_time_ms(1000);
}

static void flush_converter_to_ring(uint8_t idx) {
  while (s_host2ump[idx].availableUMP()) {
    uint32_t ump = s_host2ump[idx].readUMP();
    if (!ump_ring_push(ump)) {
      log_ring_drop(ump);
    }
  }
}

static void reset_converters(uint8_t idx, uint8_t group) {
  s_host2ump[idx] = bytestreamToUMP();
  s_host2ump[idx].defaultGroup = group;
  s_host2ump[idx].outputMIDI2  = false; // Type 0x2 MIDI 1.0 CVM (RPN/NRPN stay as CC)
  s_device2host[idx] = umpToBytestream();
}

//--------------------------------------------------------------------+
// USB MIDI 1.0 packet → bytestream → UMP
//--------------------------------------------------------------------+

static uint8_t cin_payload_len(uint8_t cin) {
  static const uint8_t k_lens[16] = {
      0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1,
  };
  return k_lens[cin & 0x0F];
}

static bool is_filtered_realtime(uint8_t b) {
  // Phase 6: Active Sensing must not enter converters (SysEx-safe + traffic cut)
  return b == MIDI_STATUS_SYSREAL_ACTIVE_SENSING;
}

static void convert_usb_midi_packet(uint8_t idx, const uint8_t packet[4]) {
  const uint8_t cin = (uint8_t)(packet[0] & 0x0F);
  const uint8_t len = cin_payload_len(cin);
  if (len == 0) {
    return;
  }

  if (packet[1] == 0 && packet[2] == 0 && packet[3] == 0 && cin == 0) {
    return;
  }

  for (uint8_t i = 0; i < len; i++) {
    const uint8_t b = packet[1 + i];
    if (is_filtered_realtime(b)) {
      continue;
    }
    s_host2ump[idx].bytestreamParse(b);
  }

  flush_converter_to_ring(idx);
}

static void process_usb_host_midi_rx(uint8_t idx) {
  if (!tuh_midi_mounted(idx)) {
    return;
  }

  uint8_t packet[4];
  while (tuh_midi_packet_read(idx, packet)) {
    convert_usb_midi_packet(idx, packet);
  }
}

//--------------------------------------------------------------------+
// Phase 5/6: UMP → Host MIDI 1.0 (Group-routed)
//--------------------------------------------------------------------+

bool usb_host_midi_first_tx_idx(uint8_t *idx_out) {
  if (idx_out == NULL) {
    return false;
  }
  for (uint8_t i = 0; i < CFG_TUH_MIDI; i++) {
    if (s_host_midi[i].mounted && s_host_midi[i].tx_cable_count > 0 && tuh_midi_mounted(i)) {
      *idx_out = i;
      return true;
    }
  }
  return false;
}

bool usb_host_midi_tx_idx_for_group(uint8_t group, uint8_t *idx_out) {
  if (idx_out == NULL) {
    return false;
  }
  for (uint8_t i = 0; i < CFG_TUH_MIDI; i++) {
    if (s_host_midi[i].mounted && s_host_midi[i].tx_cable_count > 0 && tuh_midi_mounted(i) &&
        s_host_midi[i].group == group) {
      *idx_out = i;
      return true;
    }
  }
  return usb_host_midi_first_tx_idx(idx_out);
}

void usb_host_midi_send_ump(uint32_t ump) {
  const uint8_t group = ump_word_group(ump);
  uint8_t idx = 0;
  if (!usb_host_midi_tx_idx_for_group(group, &idx)) {
    return;
  }

  s_device2host[idx].UMPStreamParse(ump);
  while (s_device2host[idx].availableBS()) {
    uint8_t byte = s_device2host[idx].readBS();
    if (is_filtered_realtime(byte)) {
      continue;
    }
    uint32_t n = tuh_midi_stream_write(idx, 0 /* cable */, &byte, 1);
    if (n == 0) {
      if (time_reached(s_next_drop_log)) {
        printf("MIDI Host TX full, dropping byte 0x%02X (idx=%u)\r\n", byte, idx);
        s_next_drop_log = make_timeout_time_ms(1000);
      }
      break;
    }
#if USB_HOST_MIDI_DEBUG_REVERSE
    printf("UMP → Host MIDI1: 0x%02X (idx=%u group=%u)\r\n", byte, idx, group);
#endif
  }
}

void usb_host_midi_flush_tx(void) {
  for (uint8_t i = 0; i < CFG_TUH_MIDI; i++) {
    if (s_host_midi[i].mounted && s_host_midi[i].tx_cable_count > 0 && tuh_midi_mounted(i)) {
      (void)tuh_midi_write_flush(i);
    }
  }
}

//--------------------------------------------------------------------+
// Phase 4: Host UMP ring → PC USB Device (+ optional forward)
//--------------------------------------------------------------------+

static void notify_ump_forward(uint32_t ump) {
  if (s_ump_forward != NULL) {
    s_ump_forward(ump);
  }
}

static void bridge_host_to_ump_device(void) {
  if (!tud_ump_n_mounted(0)) {
    return;
  }

  while (ump_ring_count() > 0) {
    if (tud_ump_n_writeable(0) < 1) {
      break;
    }

    uint32_t ump;
    if (!ump_ring_pop(&ump)) {
      break;
    }

    notify_ump_forward(ump);

    uint16_t written = tud_ump_write(0, &ump, 1);
    if (written == 0) {
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
    notify_ump_forward(ump);
    printf("Host MIDI → UMP: 0x%08lX (PC not mounted)\r\n", (unsigned long)ump);
  }
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void usb_host_midi_init(void) {
  memset(s_host_midi, 0, sizeof(s_host_midi));
  ump_ring_clear();
  s_ump_forward = NULL;
  s_next_drop_log = get_absolute_time();
  for (uint8_t i = 0; i < CFG_TUH_MIDI; i++) {
    reset_converters(i, i);
  }
}

void usb_host_midi_set_ump_forward(usb_host_ump_forward_cb_t cb) {
  s_ump_forward = cb;
}

bool usb_host_midi_is_mounted(uint8_t idx) {
  if (idx >= CFG_TUH_MIDI) {
    return false;
  }
  return s_host_midi[idx].mounted;
}

bool usb_host_midi_any_mounted(void) {
  for (uint8_t i = 0; i < CFG_TUH_MIDI; i++) {
    if (s_host_midi[i].mounted) {
      return true;
    }
  }
  return false;
}

uint8_t usb_host_midi_mounted_count(void) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < CFG_TUH_MIDI; i++) {
    if (s_host_midi[i].mounted) {
      n++;
    }
  }
  return n;
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
    bridge_host_to_ump_device();
  } else {
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

  // Phase 6: idx → UMP Group (0-based). Hub can mount up to CFG_TUH_MIDI devices.
  const uint8_t group = idx;

  usb_host_midi_dev_t *dev = &s_host_midi[idx];
  memset(dev, 0, sizeof(*dev));
  dev->mounted          = true;
  dev->daddr            = mount_cb_data->daddr;
  dev->bInterfaceNumber = mount_cb_data->bInterfaceNumber;
  dev->rx_cable_count   = mount_cb_data->rx_cable_count;
  dev->tx_cable_count   = mount_cb_data->tx_cable_count;
  dev->group            = group;

  reset_converters(idx, group);

  uint16_t vid = 0;
  uint16_t pid = 0;
  if (tuh_vid_pid_get(mount_cb_data->daddr, &vid, &pid)) {
    dev->vid = vid;
    dev->pid = pid;
  }

  printf("MIDI Host mount: idx=%u group=%u daddr=%u itf=%u VID=%04X PID=%04X rx=%u tx=%u\r\n",
         idx,
         dev->group,
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
  const uint8_t group = dev->group;

  printf("MIDI Host umount: idx=%u group=%u daddr=%u VID=%04X PID=%04X\r\n",
         idx, group, dev->daddr, dev->vid, dev->pid);

  memset(dev, 0, sizeof(*dev));
  reset_converters(idx, idx);

  // Phase 6: flush pending UMP for this device only; clear all if last device
  if (usb_host_midi_mounted_count() == 0) {
    ump_ring_clear();
  } else {
    ump_ring_drop_group(group);
  }
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

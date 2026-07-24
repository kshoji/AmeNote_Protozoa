/*
 * DIN_Bridge TinyUSB configuration — Device (RHPort0) + Host (RHPort1 / PIO USB)
 *
 * Common/include/tusb_config.h は他 UUT と共有のため変更しない。
 * 本ファイルを target_include_directories で最優先参照する。
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
  #error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS               OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG            0
#endif

//--------------------------------------------------------------------
// Root hub ports — Dual role
//--------------------------------------------------------------------

// RHPort 0: Device（MIDI 2.0 / UMP として PC 接続）
#ifndef BOARD_DEVICE_RHPORT_NUM
#define BOARD_DEVICE_RHPORT_NUM   0
#endif
#define BOARD_TUD_RHPORT          BOARD_DEVICE_RHPORT_NUM
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

// RHPort 1: Host（PIO USB — USB MIDI 1.0 デバイス接続）
#ifndef BOARD_HOST_RHPORT_NUM
#define BOARD_HOST_RHPORT_NUM     1
#endif
#define BOARD_TUH_RHPORT          BOARD_HOST_RHPORT_NUM
#define CFG_TUSB_RHPORT1_MODE     (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)

#define CFG_TUD_ENABLED           1
#define CFG_TUH_ENABLED           1

// RP2040: Host は Pico-PIO-USB を使用
#define CFG_TUH_RPI_PIO_USB       1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION（既存 UMP / MIDI 2.0 を維持）
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

#define CFG_TUD_CDC               1
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0  // UMP は tusb_ump アプリドライバ
#define CFG_TUD_VENDOR            0
#define CFG_TUD_UMP               1

#define CFG_TUD_CDC_RX_BUFSIZE    256
#define CFG_TUD_CDC_TX_BUFSIZE    256

#define CFG_TUD_UMP_RX_BUFSIZE    512
#define CFG_TUD_UMP_TX_BUFSIZE    512

//--------------------------------------------------------------------
// HOST CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUH_HUB               1
#define CFG_TUH_DEVICE_MAX        4
#define CFG_TUH_MIDI              (CFG_TUH_DEVICE_MAX)

// Host 側で使わないクラスは無効化（フットプリント削減）
#define CFG_TUH_CDC               0
#define CFG_TUH_HID               0
#define CFG_TUH_MSC               0
#define CFG_TUH_VENDOR            0

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */

# DIN_Bridge USB Host 機能追加 — 実行計画

## 1. 目的

`UUT/DIN_Bridge` を **USB MIDI 1.0 ↔ UMP ↔ USB MIDI 2.0** の双方向ブリッジデバイスとして完成させる。

USB Host ポートに接続した **USB MIDI 1.0 デバイス**（キーボード、コントローラ、音源等）と PC 側 **USB MIDI 2.0（UMP）** の間で、双方向に変換・転送する。

| 方向 | 経路 | 変換 |
|------|------|------|
| 順方向 | Host デバイス → PC | USB MIDI 1.0 → UMP（`bytestreamToUMP`）→ `tud_ump_write` |
| **逆方向** | **PC → Host デバイス** | **`tud_ump_read` → UMP → MIDI 1.0（`umpToBytestream`）→ `tuh_midi_stream_write`** |

### 1.1 最終製品像

本プロジェクトのゴールは、単なる Host 認識ではなく、以下の双方向変換パイプラインを持つブリッジデバイスの完成である。

```
[USB MIDI 1.0 デバイス] ←──USB Host──→ [RP2040 DIN_Bridge] ←──USB Device (UMP)──→ [PC / DAW]
   (キーボード・音源等)         │              ↑
        MIDI 1.0 パケット       │         MIDI 2.0 として認識
                               │    (Alternate Setting #1)
                               ↕
                        [DIN MIDI 5ピン]（既存機能、オプション経路）
```

**データフロー（双方向）:**

```
 順方向:  [MIDI 1.0 デバイス] ─MIDI1.0─→ Host ─bytestreamToUMP─→ UMP ─tud_ump_write─→ [PC]
 逆方向:  [PC] ─tud_ump_read─→ UMP ─umpToBytestream─→ MIDI1.0 ─tuh_midi_stream_write─→ [MIDI 1.0 デバイス]
```

**PC から見た動作:**

- DIN_Bridge 自身は **USB MIDI 2.0 対応デバイス**（`usb_descriptors.cpp` の UMP デスクリプタ）として列挙される
- Host ポートに接続した USB MIDI 1.0 デバイスの演奏データが、UMP に変換されて PC の DAW に届く
- **PC / DAW から送信した UMP は MIDI 1.0 に変換され、Host ポートの USB MIDI デバイス（音源等）へ送出される**（逆方向ブリッジ、Phase 5）

### 1.2 フェーズ全体像

| Phase | 名称 | ゴール | 状態 |
|-------|------|--------|------|
| 0 | 前提確認 | ハードウェア・SDK の準備 | **完了** |
| 1 | ビルド基盤 | Device + Host デュアルロールでビルド | **完了** |
| 2 | デバイス認識 | USB Host で MIDI 1.0 デバイスを列挙 | **完了** |
| 3 | **MIDI 1.0 → UMP 変換** | Host 受信データを UMP 化し内部バッファへ（**コア機能**） | **完了** |
| 4 | **MIDI 2.0 デバイスとしての動作確立** | Host→PC の UMP 送出を検証 | **完了** |
| 5 | **逆方向（UMP→MIDI1.0）・DIN 統合** | **PC→Host デバイス送出** + DIN 双方向 | **完了** |
| 6 | 安定化・拡張 | エラー処理、複数デバイス、長期運用 | **完了** |

---

## 2. 現状分析

### 2.1 既存実装 (`main.cpp`)

| 項目 | 内容 |
|------|------|
| MCU | RP2040（Pico SDK） |
| USB Device | TinyUSB + `tusb_ump`（**UMP / MIDI 2.0 デスクリプタ済み**） |
| USB Host | **未実装** |
| DIN MIDI | PIO UART（GPIO12=TX, GPIO13=RX, 31250 baud） |
| メインループ | `tud_task()` のみ |
| データフロー | PC ↔ USB UMP ↔ DIN（4 バイト UMP 単位） |
| 変換ライブラリ | `bytestreamToUMP` / `umpToBytestream`（include 済み、**未使用**） |

### 2.2 関連ファイル

| ファイル | 役割 |
|----------|------|
| `main.cpp` | アプリ本体、DIN ↔ USB Device ブリッジ |
| `usb_descriptors.cpp` | USB MIDI 2.0 デスクリプタ（Alternate Setting #0=MIDI1, #1=MIDI2） |
| `CMakeLists.txt` | ビルド設定（`tinyusb_device` のみリンク） |
| `Common/include/tusb_config.h` | **全 UUT 共通** TinyUSB 設定（Device のみ） |
| `lib/tusb_ump/` | USB **Device** 向け UMP ドライバ（MIDI 1.0 ↔ UMP 変換を内包） |
| `lib/AM_MIDI2.0Lib/` | `bytestreamToUMP` / `umpToBytestream`（Host 側変換に使用） |

### 2.3 不足している要素

- TinyUSB **Host** スタックの有効化・リンク
- USB MIDI 1.0 Host からの受信（`tuh_midi_*`）
- **USB MIDI 1.0 バイトストリーム → UMP 変換**（`bytestreamToUMP`）
- 変換後 UMP の **`tud_ump_write()` による PC への送出**
- PC からの UMP を Host 側 MIDI 1.0 へ戻す逆変換（`umpToBytestream`）
- Host ポート用ハードウェア構成

---

## 3. ハードウェア上の制約と方針

### 3.1 RP2040 の USB コントローラ

RP2040 には **ネイティブ USB コントローラが 1 つ** しかない。

- 現在: ネイティブ USB = **Device**（PC 接続用 micro-USB、MIDI 2.0 として列挙）
- Host を追加する場合: **Device と Host を同一コントローラでは同時運用不可**

### 3.2 推奨構成（デュアルロール）

| RHPort | モード | 物理ポート | 用途 |
|--------|--------|------------|------|
| RHPort 0 | Device | ネイティブ USB（micro-USB） | PC 接続 — **MIDI 2.0 デバイスとして動作** |
| RHPort 1 | Host | **PIO USB**（GPIO 2 本） | USB MIDI 1.0 デバイス接続 |

**PIO USB** を使う場合の参考:

- [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)
- CPU クロックは **120 MHz または 240 MHz**（120 MHz の倍数）が必要
- Host 側 D+/D- 用 GPIO: ProtoZOA では **GP20 (D+) / GP21 (D-)**（J13-25 / J13-23）を採用（Phase 0 確定）

> **Phase 0 確認済み:** ProtoZOA に専用 USB-A Host コネクタはない。PIO USB は J13 の GP20/GP21（必要なら GP22=VBUS EN）へ外部 USB-A を配線して構築する。

### 3.3 代替構成

| 方式 | メリット | デメリット |
|------|----------|------------|
| PIO USB（推奨） | Device + Host 同時運用 | GPIO 2 本 + 120/240 MHz クロック制約 |
| MAX3421E（SPI Host） | ネイティブ USB を Device 専用にできる | 追加 IC・配線が必要 |
| Host のみ（Device 無効） | 実装が単純 | MIDI 2.0 デバイスとして PC に提供できなくなる |

本計画では **PIO USB による RHPort1 Host** を前提とする。

---

## 4. アーキテクチャ設計

### 4.1 変換パイプライン（本プロジェクトの核心）

```
┌─────────────────────────────────────────────────────────────────┐
│                     RP2040 DIN_Bridge                           │
│                                                                 │
│  [USB Host]          変換レイヤー              [USB Device]      │
│                                                                 │
│  tuh_midi_read()  →  bytestreamToUMP  →  tud_ump_write()       │
│  (USB MIDI 1.0        (AM_MIDI2.0Lib)     (tusb_ump)           │
│   4-byte packet)                              ↓                 │
│                                          PC へ UMP 送出          │
│                                          (MIDI 2.0 として認識)    │
│                                                                 │
│  tuh_midi_stream_write() ← umpToBytestream ← tud_ump_read()    │
│  (逆方向: PC/DAW → Host デバイス)                               │
└─────────────────────────────────────────────────────────────────┘
```

**変換の責務分担:**

| 区間 | 担当 | 形式 |
|------|------|------|
| Host デバイス → RP2040 | TinyUSB `tuh_midi_*` | USB MIDI 1.0 パケット（CIN + データ） |
| MIDI 1.0 → UMP | `bytestreamToUMP` | 32-bit UMP ワード |
| RP2040 → PC | `tusb_ump` / `tud_ump_write()` | UMP（USB MIDI 2.0 エンドポイント） |
| PC → RP2040 | `tud_ump_read()` | UMP |
| UMP → MIDI 1.0 | `umpToBytestream` | バイトストリーム |
| RP2040 → Host デバイス | `tuh_midi_stream_write()` + `flush` | USB MIDI 1.0 パケット |

> **注:** `tusb_ump` は Device 側で PC との MIDI 1.0 / 2.0 切り替えを処理する。Host 側は TinyUSB 標準の **USB MIDI 1.0 Host のみ** 対応。Host ポートに接続するデバイスは USB MIDI 1.0 を前提とする。

### 4.2 ソフトウェア構成

```
main.cpp
├── 初期化
│   ├── pio_rx_init / pio_tx_init     (既存 DIN)
│   ├── bytestreamToUMP / umpToBytestream インスタンス
│   ├── tusb_init()                   (Device + Host)
│   └── pio_usb / Host 5V 電源 GPIO
├── メインループ
│   ├── tud_task()                    (USB Device: MIDI 2.0 応答)
│   ├── tuh_task()                    (USB Host: MIDI 1.0 受信)
│   ├── bridge_host_to_ump_device()   (Phase 4: Host→PC)
│   ├── bridge_ump_device_to_host()   (Phase 5: PC→Host 逆方向 ★)
│   ├── bridge_usb_device_to_din()    (既存 / Phase 5)
│   └── bridge_din_to_usb_device()    (既存 / Phase 5)
└── コールバック
    ├── tuh_midi_mount_cb()
    ├── tuh_midi_umount_cb()
    └── tuh_midi_rx_cb()
```

### 4.3 新規・変更ファイル（予定）

| ファイル | 操作 | 内容 |
|----------|------|------|
| `tusb_config.h` | **新規**（DIN_Bridge 専用） | Device + Host デュアルロール設定 |
| `usb_host_midi.cpp` | 新規（推奨） | Host コールバック・MIDI 1.0 ↔ UMP 変換 |
| `usb_host_midi.h` | 新規（推奨） | Host API 宣言 |
| `main.cpp` | 変更 | 変換パイプライン統合 |
| `CMakeLists.txt` | 変更 | `tinyusb_host`, `pico_pio_usb` 追加 |

> `Common/include/tusb_config.h` は他 UUT と共有のため **変更しない**。DIN_Bridge の `target_include_directories` で **プロジェクトローカルの `tusb_config.h` を先に参照** させる。

---

## 5. 実装フェーズ

### Phase 0: 前提確認（ハードウェア・SDK） — **完了**

- [x] ボードの Host 用 USB コネクタ / PIO USB GPIO ピンを確認
- [x] Host 5V 電源制御 GPIO の有無を確認
- [x] Pico SDK **2.1 以降** を使用（TinyUSB MIDI Host サポート）
- [x] TinyUSB に `midi_host.h` が含まれることを確認
- [x] CPU クロックを 120 MHz または 240 MHz に設定
- [x] PC 側 DAW / MIDI 2.0 対応ツール（MIDI 2.0 動作確認用）を準備

**完了条件:** Host ポートに USB MIDI 1.0 デバイスを物理接続できる状態 — **配線計画確定済み（下記 Phase 0 確認結果）**

#### Phase 0 確認結果（2026-07-24）

##### 1. Host 用 USB / PIO USB GPIO

| 項目 | 結果 |
|------|------|
| 専用 USB-A Host コネクタ | **なし**（ProtoZOA 基板上に未実装） |
| 公式想定の Host 手段 | UUT Pico の micro-USB に **OTG → USB-A アダプタ**（Device と排他。デュアルロール不可） |
| 本計画の方式 | **PIO USB（RHPort1）** で Device + Host 同時運用 |
| 利用可能な UUT GPIO（J13） | **GP19 / GP20 / GP21 / GP22**（User Manual J13） |

**採用ピンアサイン（TinyUSB RP2040 デフォルトと一致）:**

| 信号 | GPIO | J13 ピン | 備考 |
|------|------|----------|------|
| USB D+ | **GP20** | J13-25 | `PICO_DEFAULT_PIO_USB_DP_PIN` |
| USB D- | **GP21** | J13-23 | D+ の隣ピン（`pin_dp + 1`）必須 |
| VBUS enable（任意） | **GP22** | J13-21 | `PICO_DEFAULT_PIO_USB_VBUSEN_PIN`（外部 FET 等を接続する場合） |
| GND | — | J13-3/6/7/17/19/29 | 共通 GND |
| +5V（バス給電） | — | J13-2（UUT 5V） | 下記 5V 節を参照 |

> **物理接続:** J13 から USB-A レセプタクル（またはブレイクアウト）へ D+/D-/GND/+5V を配線すれば、USB MIDI 1.0 デバイスを Host ポートとして接続できる。既存 DIN（GP12/GP13）・UART0（GP0/GP1）とは非干渉。

> **非推奨:** micro-USB OTG アダプタのみの Host 化は、PC 向け MIDI 2.0 Device（ネイティブ USB）と同時運用できないため本計画では使わない。

##### 2. Host 5V 電源制御 GPIO

| 項目 | 結果 |
|------|------|
| 基板上の専用 VBUS スイッチ IC | **なし** |
| 5V 供給源 | ProtoZOA 共有 5V バス（J9 / J21）、または J13-2（UUT Pico 5V） |
| GPIO による 5V enable | **標準回路なし**。GP22 を enable に使う場合は外部 FET / ロードスイッチ追加が必要 |
| 推奨運用 | 外部 DC（9–12 V）でボード給電し、Host 側 5V は J13-2 から常時供給。バスパワー機器で電流が大きい場合は User Manual どおり外部電源必須 |

##### 3. Pico SDK / TinyUSB / `midi_host.h`

| 項目 | 結果 |
|------|------|
| 本機の Pico SDK | **2.2.0**（`~/.pico-sdk/sdk/2.2.0`）— **2.1 以降の要件を満たす** |
| 同梱 TinyUSB | **0.18.0** |
| `midi_host.h`（SDK 同梱） | **未同梱**（`class/midi/` に device のみ。`CFG_TUH_MIDI` スタブのみ存在） |
| 上流 TinyUSB | master / **0.19.0 以降** に `midi_host.c/h` あり（0.21.0 が最新系） |
| Pico-PIO-USB | SDK 同梱 TinyUSB 配下に **未インストール**（`hw/mcu/raspberry_pi/Pico-PIO-USB` 不在） |

**Phase 1 での必須対応:**

1. TinyUSB を **0.19+（推奨: 0.21.0 または master）** に更新（`PICO_TINYUSB_PATH` 上書き、または SDK 内 submodule 更新）
2. [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) を TinyUSB が参照するパスへ配置（参考: 0.7.1）
3. `midi_host.h` の存在をビルド前に再確認

##### 4. CPU クロック

| 項目 | 結果 |
|------|------|
| 現状 `main.cpp` | `set_sys_clock_*` **未設定**（デフォルト 125 MHz） |
| PIO USB 要件 | **120 MHz または 240 MHz**（120 の倍数） |
| Phase 1 以降の方針 | Host 初期化前に `set_sys_clock_khz(120000, true)` を実行 |

##### 5. PC 側 MIDI 2.0 検証ツール

| ツール | 用途 | 状態 |
|--------|------|------|
| [MIDI 2.0 Workbench](https://github.com/midi2-dev/MIDI2.0Workbench) | UMP / MIDI 2.0 プロトタイプ検証（ProtoZOA 公式推奨） | 準備対象として確定 |
| macOS（Monterey 以降） | OS ネイティブ MIDI 2.0 列挙確認 | 利用可（QuickStartGuide 記載） |
| Ableton Live 12+ / Cubase 14+ 等 | DAW 実演奏確認（Phase 4） | 任意・利用環境に応じて |
| シリアルモニタ | Phase 3 UMP ダンプ | 標準ツールで可 |
| USB MIDI 1.0 テストデバイス | Host 接続試験（キーボード等） | 利用者側で用意 |

---

### Phase 1: ビルド基盤 — Host スタック有効化 — **完了**

#### 5.1.1 `UUT/DIN_Bridge/tusb_config.h` を新規作成

`Common/include/tusb_config.h` をベースに、Device + Host デュアルロール設定を追加。

```c
// RHPort 0: Device（MIDI 2.0 デバイスとして PC 接続）
#define BOARD_DEVICE_RHPORT_NUM   0
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

// RHPort 1: Host（USB MIDI 1.0 デバイス接続）
#define BOARD_HOST_RHPORT_NUM     1
#define CFG_TUSB_RHPORT1_MODE     (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)

#define CFG_TUH_ENABLED           1
#define CFG_TUH_RPI_PIO_USB       1
#define CFG_TUH_DEVICE_MAX        4
#define CFG_TUH_HUB               1
#define CFG_TUH_MIDI              (CFG_TUH_DEVICE_MAX)
#define CFG_TUH_ENUMERATION_BUFSIZE 256

// Device 設定（既存 UMP / MIDI 2.0 を維持）
#define CFG_TUD_CDC               1
#define CFG_TUD_UMP               1
```

#### 5.1.2 `CMakeLists.txt` 変更

- `target_include_directories` で DIN_Bridge ローカルを最優先
- `tinyusb_host`, `tinyusb_pico_pio_usb` をリンク
- ルート `CMakeLists.txt` で `PICO_TINYUSB_PATH=lib/tinyusb`（0.21.0）、`PICO_PIO_USB_PATH=lib/Pico-PIO-USB`（0.7.1）を設定

#### 5.1.3 ビルド確認

- [x] Device + Host デュアルロールでビルド成功
- [x] 既存の MIDI 2.0 デスクリプタ（`usb_descriptors.cpp`）がそのまま有効

**完了条件:** ビルド成功、PC への MIDI 2.0 デバイス列挙に影響なし — **達成**（`UUT_DIN_BRIDGE.uf2` 生成、`midi_host` / `pio_usb_host` / `tud_ump_*` リンク確認済み）

#### Phase 1 実施結果（2026-07-24）

| 項目 | 内容 |
|------|------|
| 新規 | `UUT/DIN_Bridge/tusb_config.h`（Device+Host） |
| 変更 | `UUT/DIN_Bridge/CMakeLists.txt`、`main.cpp`（120 MHz・PIO USB init・`tuh_task`） |
| 変更 | ルート `CMakeLists.txt`（TinyUSB / Pico-PIO-USB パス） |
| 依存追加 | `lib/tinyusb` **0.21.0**、`lib/Pico-PIO-USB` **0.7.1** |
| 互換対応 | `lib/tusb_ump` を TinyUSB 0.21 API に適合（`usbd_edpt_xfer` / `tu_fifo_config` / driver 構造体、`ump.h` と `midi.h` の定数衝突回避） |
| PIO 割当 | DIN MIDI = **pio0**、PIO USB Host = **pio1**、D+ = **GP20** |
| ビルド成果物 | `build-din-bridge/UUT/DIN_Bridge/UUT_DIN_BRIDGE.uf2` |

---

### Phase 2: USB MIDI 1.0 デバイス認識 — **完了**

Host ポートに接続した USB MIDI 1.0 デバイスを列挙・認識する。

#### 5.2.1 Host 初期化

> **注:** Phase 1 で `usb_dual_init()`（クロック・`tuh_configure`・`tusb_init`）は実装済み。Phase 2 では mount/unmount コールバックと認識ログを追加する。

```cpp
#define HOST_PIN_DP  20  // Phase 0: J13-25 / GP20。D- は GP21（自動で +1）

void usb_host_init(void) {
    set_sys_clock_khz(120000, true);  // PIO USB 要件
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = HOST_PIN_DP;
    tuh_configure(BOARD_HOST_RHPORT_NUM, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tusb_init();
}
```

#### 5.2.2 認識コールバック

> **API 注記（TinyUSB 0.21）:** unmount コールバック名は `tuh_midi_umount_cb`（`unmount` ではない）。マウント情報型は `tuh_midi_mount_cb_t`。

```cpp
void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t* mount_cb_data) {
    // VID/PID, ケーブル数, エンドポイント情報を記録
    // tuh_vid_pid_get(daddr, &vid, &pid) で VID/PID を取得し printf
}

void tuh_midi_umount_cb(uint8_t idx) {
    // mounted 状態をクリアしログ出力
}

void tuh_midi_rx_cb(uint8_t idx, uint32_t xferred_bytes) {
    // Phase 2: RX FIFO を drain（後続 bulk IN 停滞防止）
    // Phase 3 で UMP 変換処理を呼び出す
}
```

#### 5.2.3 メインループ

```cpp
while (true) {
    tud_task();
    tuh_task();
}
```

**完了条件:** USB MIDI 1.0 デバイス接続時に `tuh_midi_mount_cb` が呼ばれ、VID/PID が確認できる — **実装完了**（実機認識テストは利用者側で実施）

#### Phase 2 実施結果（2026-07-24）

| 項目 | 内容 |
|------|------|
| 新規 | `UUT/DIN_Bridge/usb_host_midi.cpp` / `usb_host_midi.h` |
| 変更 | `main.cpp`（`stdio_init_all`・`usb_host_midi_init`） |
| 変更 | `CMakeLists.txt`（ソース追加、`pico_enable_stdio_usb`） |
| コールバック | `tuh_midi_mount_cb` / `tuh_midi_umount_cb` / `tuh_midi_rx_cb` |
| ログ | CDC シリアルへ `VID`/`PID`・cable 数を出力 |
| ビルド成果物 | `build-din-bridge/UUT/DIN_Bridge/UUT_DIN_BRIDGE.uf2` |

**実機確認手順（7.1）:** ファーム書き込み後、CDC シリアルを開き Host ポートに USB MIDI 1.0 デバイスを接続すると `MIDI Host mount: ... VID=xxxx PID=xxxx` が出力される。

---

### Phase 3: USB MIDI 1.0 → UMP 変換（コア機能） — **完了**

**本プロジェクトの最重要フェーズ。** Host ポートから受信した USB MIDI 1.0 データを UMP に変換し、内部バッファに蓄積する。

#### 5.3.1 受信・変換処理

> **API 注記:** TinyUSB 0.21 では `tuh_midi_packet_read()`。パケット byte0 は `(cable << 4) | CIN` のため **CIN = `packet[0] & 0x0F`**。

```cpp
bytestreamToUMP host2ump;

void process_usb_host_midi_rx(uint8_t idx) {
    if (!tuh_midi_mounted(idx)) return;

    uint8_t packet[4];
    while (tuh_midi_packet_read(idx, packet)) {
        uint8_t cin = packet[0] & 0x0F;
        uint8_t len = cin_payload_len(cin); // CIN → 有効バイト数
        for (uint8_t i = 0; i < len; i++) {
            uint8_t b = packet[1 + i];
            if (b == 0xFE) continue; // Active Sensing スキップ
            host2ump.bytestreamParse(b);
        }
        // 変換済み UMP をリングバッファへ
    }
}

bool usb_host_midi_pop_ump(uint32_t* ump); // Phase 4 送出用
```

#### 5.3.2 UMP バッファリング

- Host からの変換 UMP は **128 ワードのリングバッファ** で保持（`usb_host_midi.cpp`）
- `tud_ump_n_mounted(0)` が true のときのみ PC へ送出（Phase 4）
- Phase 3 では `usb_host_midi_task()` がリングを drain してシリアルダンプ

#### 5.3.3 デバッグ確認（PC 送出前）

Phase 3 単体では `printf` で UMP をダンプし、変換正確性を確認:

```
Host MIDI → UMP: 0x20903C40 (MIDI 1.0 Channel Voice UMP — Note On C4 例)
```

**完了条件:** Host キーボードの演奏が正しい UMP ワードに変換される（シリアルログで検証） — **実装完了**（実機演奏検証は利用者側で実施）

#### Phase 3 実施結果（2026-07-24）

| 項目 | 内容 |
|------|------|
| 変更 | `usb_host_midi.cpp` / `.h`（変換・リング・`pop_ump` / `task`） |
| 変更 | `main.cpp`（`usb_host_midi_task()` をメインループへ追加） |
| 変換 | `bytestreamToUMP`（`outputMIDI2=false` → Type 0x2 MIDI 1.0 CVM） |
| 受信 | `tuh_midi_rx_cb` → CIN 解釈 → `bytestreamParse` → リング |
| フィルタ | Active Sensing (`0xFE`)、CIN 0/1、ゼロパディングパケット |
| デバッグ | CDC: `Host MIDI → UMP: 0x........` |
| Phase 4 準備 | `usb_host_midi_pop_ump()` 公開済み |
| ビルド成果物 | `build-din-bridge/UUT/DIN_Bridge/UUT_DIN_BRIDGE.uf2` |

---

### Phase 4: MIDI 2.0 デバイスとして PC へ送出 — **完了**

Phase 3 で生成した UMP を `tud_ump_write()` 経由で PC に送出し、**MIDI 2.0 デバイスとしての動作** を確立する。

#### 5.4.1 PC への UMP 送出

```cpp
void bridge_host_to_ump_device(void) {
    if (!tud_ump_n_mounted(0)) return;

    while (ump available && tud_ump_n_writeable(0) >= 1) {
        uint32_t ump;
        usb_host_midi_pop_ump(&ump);
        tud_ump_write(0, &ump, 1);
    }
}
```

#### 5.4.2 MIDI 2.0 Alternate Setting の確認

- PC の OS / DAW が Alternate Setting #1（MIDI 2.0）を選択した場合、`tud_alt_setting(0) + 1 == 2` となり UMP がそのまま送出される
- Alternate Setting #0（MIDI 1.0 互換）の場合、`tusb_ump` ドライバが内部で UMP → USB MIDI 1.0 に変換して送出（既存動作）

#### 5.4.3 メインループ統合

```cpp
while (true) {
    tud_task();
    tuh_task();
    usb_host_midi_task(); // Phase 4: Host → PC bridge（未接続時はシリアルダンプ）
    // 既存 DIN ↔ PC …
}
```

#### 5.4.4 動作確認ポイント

| 確認項目 | 期待結果 |
|----------|----------|
| PC が DIN_Bridge を MIDI 2.0 デバイスとして認識 | Alternate Setting #1 が選択可能 |
| Host キーボード演奏 → PC DAW | DAW の MIDI 2.0 入力にノートイベント表示 |
| UMP フォーマット | MIDI 1.0 Channel Voice UMP（Type 0x2）として正しく届く |
| タイミング | 演奏遅延が実用範囲内（目安: < 10 ms） |

**完了条件:** Host ポートの USB MIDI 1.0 デバイスの演奏が、PC 上の MIDI 2.0 対応 DAW に UMP として届く — **実装完了**（実機 DAW 検証は利用者側で実施）

#### Phase 4 実施結果（2026-07-24）

| 項目 | 内容 |
|------|------|
| 変更 | `usb_host_midi.cpp` — `bridge_host_to_ump_device()` 追加 |
| 変更 | `usb_host_midi_task()` — PC mount 時は `tud_ump_write`、未接続時は Phase 3 ダンプ |
| 流量制御 | `tud_ump_n_writeable()` で TX FIFO 空きを確認してから pop |
| デバッグ | `USB_HOST_MIDI_DEBUG_UMP=1` で PC 送出 UMP を printf（既定オフ） |
| ビルド成果物 | `build-din-bridge/UUT/DIN_Bridge/UUT_DIN_BRIDGE.uf2` |

---

### Phase 5: 逆方向（UMP → MIDI 1.0）・DIN 統合 — **完了**

PC（MIDI 2.0 / UMP）から受信したデータを **MIDI 1.0 に変換**し、Host ポートの USB MIDI デバイスへ送出する。あわせて DIN ポートとの双方向ルーティングを統合する。

> **本フェーズの主機能（ユーザー要求対応）:**  
> PC が DIN_Bridge へ送った UMP → `umpToBytestream` → USB MIDI 1.0 バイト列 → `tuh_midi_stream_write` / `tuh_midi_write_flush` → Host 接続デバイス（音源・コントローラ等）。  
> Host 側 TinyUSB は MIDI 1.0 のみのため、デバイスへは常に MIDI 1.0 パケットとして届く。

#### 5.5.1 PC → Host（UMP → USB MIDI 1.0）— **逆方向コア**

```
[PC / DAW]
    │  UMP (USB MIDI 2.0 Device EP)
    ▼
 tud_ump_read()          ← 1 回読み出しで DIN / Host へファンアウト
    │
 umpToBytestream::UMPStreamParse()   ← AM_MIDI2.0Lib
    │  MIDI 1.0 バイトストリーム
    ▼
 tuh_midi_stream_write(idx, cable, buf, n)
 tuh_midi_write_flush(idx)
    │  USB MIDI 1.0 4-byte packets
    ▼
[Host ポートの USB MIDI 1.0 デバイス]
```

```cpp
// usb_host_midi.cpp
void usb_host_midi_send_ump(uint32_t ump) {
    // first TX-capable Host idx → umpToBytestream → tuh_midi_stream_write
}
void usb_host_midi_flush_tx(void);

// main.cpp — PC 受信は単一 read で DIN + Host へ配信（二重消費回避）
void bridge_pc_ump_to_din_and_host(void);
```

**実装メモ:**

| 項目 | 方針 |
|------|------|
| 変換ライブラリ | `umpToBytestream`（`lib/AM_MIDI2.0Lib`） |
| Host 送出 API | `tuh_midi_stream_write` + `tuh_midi_write_flush`（TinyUSB 0.21） |
| 対象デバイス | TX cable 付きの先頭 mount `idx`（複数時は Phase 6 で Group / cable 割当） |
| MIDI 2.0 CVM (Type 0x4) | `umpToBytestream` が MIDI 1.0 バイトへダウン変換（仕様どおり） |
| デバッグ | `USB_HOST_MIDI_DEBUG_REVERSE=1` で「UMP → Host MIDI1」バイトダンプ（既定オフ） |

#### 5.5.2 DIN ポート統合（既存機能との共存）

| 経路 | 動作 |
|------|------|
| Host → DIN | Host→UMP 後、`usb_host_midi_set_ump_forward` → `din_write_ump_word` |
| DIN → PC | 既存: DIN RX → `tud_ump_write` |
| DIN → Host | DIN RX → UMP → `usb_host_midi_send_ump` → Host OUT |
| PC → DIN | 既存: `tud_ump_read` → DIN TX（Host と同時ファンアウト） |

#### 5.5.3 ルーティング方針

| シナリオ | デフォルト | 担当 Phase |
|----------|------------|------------|
| Host MIDI IN → PC（MIDI 2.0 UMP） | **有効** | Phase 4 |
| **PC MIDI OUT（UMP）→ Host MIDI OUT（MIDI 1.0）** | **有効** | **Phase 5（本節）** |
| Host MIDI IN → DIN OUT | 有効 | Phase 5 |
| DIN IN → PC | 有効（既存） | Phase 5 |
| DIN IN → Host MIDI OUT | 有効 | Phase 5 |

**完了条件:** PC → Host 音源でノート ON/OFF・CC が鳴ること。加えて DIN 含む全経路で双方向伝搬が正しいこと。 — **実装完了**（実機検証は利用者側で実施）

#### Phase 5 実施結果（2026-07-25）

| 項目 | 内容 |
|------|------|
| 変更 | `usb_host_midi.cpp` / `.h` — `umpToBytestream`、`send_ump` / `flush_tx`、Host→DIN forward CB |
| 変更 | `main.cpp` — `bridge_pc_ump_to_din_and_host`、`bridge_din_to_pc_and_host`、Host→DIN 登録 |
| 逆方向 | PC UMP → `umpToBytestream` → `tuh_midi_stream_write` + `flush` |
| DIN 統合 | Host→DIN / PC→DIN+Host / DIN→PC+Host（UMP ワード単位・既存 endian） |
| 流量注意 | `tud_ump_read` は 1 回のみ。DIN と Host へ同一パケットをファンアウト |
| デバッグ | `USB_HOST_MIDI_DEBUG_REVERSE=1`（既定オフ） |
| ビルド成果物 | `build-din-bridge/UUT/DIN_Bridge/UUT_DIN_BRIDGE.uf2` |

---

### Phase 6: 安定化・拡張 — **完了**

- [x] Active Sensing (0xFE) フィルタリング（Host RX / PC→Host 双方向）
- [x] SysEx 7/8 バイトストリームの長文対応（**idx ごとの** `bytestreamToUMP` / `umpToBytestream` 状態機械）
- [x] 複数 USB MIDI 1.0 デバイス対応（`idx` → UMP Group 割り当て、逆方向は Group でルーティング）
- [x] Host デバイス切断時の UMP バッファフラッシュ（該当 Group のみ削除、最終切断時は全クリア）
- [x] 接続状態 LED（**Raspberry Pi Pico オンボード LED** / `PICO_DEFAULT_LED_PIN`）
- [x] MIDI 2.0 拡張メッセージ（NRPN/RPN）: Host→PC は Type 0x2 で CC として透過。PC→Host の Type 0x4 RPN/NRPN は `umpToBytestream` が CC 列へダウン変換
- [x] 連続演奏向け安定化（リング 256、ドロップログの 1 秒レート制限）

**完了条件:** 長時間運用・複数デバイス・SysEx を含む実用シナリオで安定動作 — **実装完了**（実機ストレステストは利用者側で実施）

#### Phase 6 実施結果（2026-07-26）

| 項目 | 内容 |
|------|------|
| 変更 | `usb_host_midi.cpp` / `.h` — 複数デバイス Group、per-idx 変換器、切断フラッシュ、リング拡大 |
| 変更 | `main.cpp` — Pico オンボード LED ステータス表示 |
| Group 割当 | mount 時 `group = idx`（0-based）。PC→Host は UMP Group で対象 idx を選択 |
| SysEx | デバイスごとに独立した `bytestreamToUMP` / `umpToBytestream`（並行 SysEx でも状態干渉なし） |
| 切断処理 | `ump_ring_drop_group(group)` + 変換器リセット。最後の 1 台切断時はリング全クリア |
| Active Sensing | `0xFE` を Host RX / 逆方向の双方で破棄（SysEx 中断防止） |
| LED パターン | **点灯**: PC+Host 接続 / **遅点滅(~1Hz)**: Host 待ち / **快点滅(~5Hz)**: PC 待ち / **極遅**: 両方なし |
| RPN/NRPN | `outputMIDI2=false` 維持（Type 0x2）。逆方向 Type 0x4 はライブラリが CC#101/100/6/38 等へ展開 |
| ビルド成果物 | `build-din-bridge/UUT/DIN_Bridge/UUT_DIN_BRIDGE.uf2` |

**実機確認の目安:**

| # | 手順 | 期待結果 |
|---|------|----------|
| 1 | PC のみ接続 | LED が約 1 Hz で点滅 |
| 2 | Host に USB MIDI 接続 | LED 点灯固定、CDC に `group=N` 表示 |
| 3 | ハブで 2 台接続 | それぞれ別 `idx`/`group`、演奏が別 Group の UMP になる |
| 4 | 長い SysEx（Identity 等） | 欠落なく PC / Host へ到達 |
| 5 | Host 切断 | umount ログ、LED が Host 待ち点滅に戻る、残留 UMP クリア |
| 6 | アルペジオ / 高速 CC | 取りこぼしが実用範囲（リング full ログが出ないこと） |

---

## 6. `main.cpp` 変更概要（最終形イメージ）

```cpp
#include "class/midi/midi_host.h"
#include "pico_pio_usb.h"
#include "include/bytestreamToUMP.h"
#include "include/umpToBytestream.h"

bytestreamToUMP host2ump;       // Host MIDI 1.0 → UMP
umpToBytestream device2host;    // UMP → Host MIDI 1.0
bool host_midi_mounted[CFG_TUH_DEVICE_MAX] = {false};

int main() {
    stdio_init_all();

    pio_rx_init(pio, smRx);
    pio_tx_init(pio, smTx);
    usb_host_init();

    host2ump.defaultGroup = 0;  // Host デバイスを Group 1 にマッピング

    while (true) {
        tud_task();
        tuh_task();

        // --- Phase 3/4: Host MIDI 1.0 → UMP → PC (MIDI 2.0) ---
        for (uint8_t idx = 0; idx < CFG_TUH_DEVICE_MAX; idx++) {
            if (host_midi_mounted[idx]) {
                process_usb_host_midi_rx(idx);
            }
        }
        bridge_host_to_ump_device();

        // --- Phase 5: PC (MIDI 2.0) → Host MIDI 1.0 ---
        if (tud_ump_n_mounted(0)) {
            bridge_ump_device_to_host(0);

            // --- 既存: DIN ↔ USB Device ---
            bridge_usb_device_to_din();
            bridge_din_to_usb_device();
        }
    }
}
```

---

## 7. テスト計画

### 7.1 Phase 2（認識テスト）

| # | 手順 | 期待結果 |
|---|------|----------|
| 1 | ファーム書き込み、PC に USB Device 接続 | MIDI 2.0 デバイスとして認識（既存動作維持） |
| 2 | Host ポートに USB MIDI 1.0 キーボード接続 | mount コールバック、VID/PID 表示 |
| 3 | キーボードを抜く | unmount コールバック |
| 4 | USB ハブ経由で接続 | 正常に mount |

### 7.2 Phase 3（変換テスト）

| # | 手順 | 期待結果 |
|---|------|----------|
| 1 | Host キーボードで C4 ノート ON | シリアルログに UMP `0x20904090` 等 |
| 2 | CC#1 (Modulation) 送信 | 対応する UMP Type 0x2 が出力 |
| 3 | SysEx（Identity Request）送信 | Sysex7 UMP シーケンスが正しく生成 |
| 4 | Active Sensing 連続受信 | フィルタされ UMP に変換されない |

### 7.3 Phase 4（MIDI 2.0 デバイステスト）

| # | 手順 | 期待結果 |
|---|------|----------|
| 1 | PC DAW で DIN_Bridge を MIDI 2.0 入力として選択 | Alternate Setting #1 で接続 |
| 2 | Host キーボード演奏 | DAW の MIDI 2.0 トラックにノート記録 |
| 3 | MIDI 2.0 モニタで UMP を確認 | Type 0x2 MIDI 1.0 Channel Voice UMP |
| 4 | MIDI 1.0 互換モード（Alt #0）でも Host → PC 動作 | `tusb_ump` が UMP → MIDI 1.0 変換して送出 |

### 7.4 Phase 5（逆方向 UMP→MIDI1.0・DIN テスト）

| # | 手順 | 期待結果 |
|---|------|----------|
| 1 | PC DAW からノート ON → Host 接続音源（UMP→MIDI1.0） | **音源が鳴る（逆方向コア検証）** |
| 2 | PC から CC / Pitch Bend → Host 音源 | パラメータが追従する |
| 3 | Host 演奏 → DIN OUT | DIN モニタに MIDI 1.0 バイト列 |
| 4 | DIN IN → PC | DAW に UMP として記録 |
| 5 | DIN IN → Host 音源 | 音源が鳴る |
| 6 | 連続演奏（アルペジオ）双方向 | ノート取りこぼしなし |

### 7.5 使用ツール

- シリアルモニタ（UMP ダンプ確認）
- MIDI 2.0 対応 DAW（Ableton Live 12+, Cubase 14+ 等）または UMP モニタ
- USB MIDI 1.0 テストデバイス（キーボード、Korg microKEY 等）
- DIN MIDI モニタ（Phase 5）

---

## 8. 依存関係・リスク

| リスク | 影響 | 対策 |
|--------|------|------|
| Pico SDK / TinyUSB バージョン不足 | `tuh_midi_*` API 不在 | **解消:** SDK 2.2.0 + プロジェクト内 TinyUSB **0.21.0**（`midi_host` 含む） |
| Pico-PIO-USB 未配置 | PIO Host ビルド不可 | **解消:** `lib/Pico-PIO-USB` **0.7.1** を配置 |
| PIO USB ピン未配線 | Host ポート不動作 | Phase 0 で **GP20/21（J13）** 確定。外部 USB-A 配線が必要 |
| Host 5V 制御回路なし | バスパワー機器が動かない | J13-2 から 5V 供給 + 外部 DC。GP22 で FET 制御はオプション |
| CPU クロック 120 MHz 以外 | PIO USB 不安定 | **解消:** `main.cpp` で `set_sys_clock_khz(120000, true)` を実装 |
| `bytestreamToUMP` の Sysex 長文 | 変換欠損 | Phase 6 で Sysex 状態機械テスト |
| PC が MIDI 2.0 を選択しない | UMP 直接送出不可 | `tusb_ump` が MIDI 1.0 互換変換で代替 |
| Host 側 USB MIDI 2.0 デバイス | 1.0 Host では非対応 | スコープ外（1.0 デバイスのみ） |
| 共通 `tusb_config.h` 変更 | 他 UUT ビルド破壊 | DIN_Bridge 専用 config を別ファイル化 |
| UMP Group マッピング | 複数 Host デバイスの区別 | Phase 6 で idx → Group 割り当て |

---

## 9. 作業見積もり

| フェーズ | 内容 | 目安 |
|----------|------|------|
| Phase 0 | ハードウェア・SDK 確認 | 0.5〜1 日 |
| Phase 1 | ビルド基盤（config, CMake） | 0.5 日 |
| Phase 2 | USB MIDI 1.0 デバイス認識 | 1 日 |
| Phase 3 | **MIDI 1.0 → UMP 変換** | 1 日 |
| Phase 4 | **MIDI 2.0 デバイスとして PC へ送出** | 0.5〜1 日 |
| Phase 5 | **UMP→MIDI1.0 逆方向**・DIN 統合 | 1〜2 日 |
| Phase 6 | 安定化・テスト | 1 日 |
| **合計** | | **5.5〜7.5 日** |

---

## 10. 参考資料

- [TinyUSB MIDI Host API (`midi_host.h`)](https://github.com/hathach/tinyusb/tree/master/src/class/midi)
- [rppicomidi/usb_midi_host — RP2040 Host 例・API 解説](https://github.com/rppicomidi/usb_midi_host)
- [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)
- [Adafruit USB MIDI Host Messenger ガイド](https://learn.adafruit.com/usb-midi-host-messenger)
- 本リポジトリ `lib/tusb_ump/README.md`（Device 側 UMP ドライバ、MIDI 1.0 ↔ UMP 変換）
- 本リポジトリ `lib/AM_MIDI2.0Lib/docs/bytestreamToUMP.md`（バイトストリーム → UMP）
- 本リポジトリ `lib/AM_MIDI2.0Lib` — `umpToBytestream`（**UMP → MIDI 1.0 バイトストリーム、逆方向用**）
- [MIDI 2.0 UMP 仕様](https://midi.org/midi-2-0-specification)

---

## 11. 次のアクション

1. ~~**Phase 0:** ボードの Host USB / PIO USB ピン配置を確認~~ **完了**
2. ~~**Phase 1:** TinyUSB 0.21 + Pico-PIO-USB、`tusb_config.h` / CMake / デュアル初期化~~ **完了**
3. ~~**Phase 2:** `usb_host_midi.cpp` に mount/unmount コールバックを実装し、認識テスト~~ **完了**
4. ~~**Phase 3:** `bytestreamToUMP` による Host MIDI 1.0 → UMP 変換を実装~~ **完了**
5. ~~**Phase 4:** `tud_ump_write()` で PC へ UMP 送出、MIDI 2.0 デバイス動作を確認~~ **完了**
6. ~~**Phase 5:** `umpToBytestream` + `tuh_midi_stream_write` で **PC UMP → Host MIDI 1.0 デバイス** 送出、DIN 統合~~ **完了**
7. ~~**Phase 6:** 安定化・拡張（複数デバイス、SysEx 長文、Pico LED、ストレステスト向け強化）~~ **完了**

実行計画の実装フェーズはすべて完了。以降は実機での長期運用・DAW 検証が中心。

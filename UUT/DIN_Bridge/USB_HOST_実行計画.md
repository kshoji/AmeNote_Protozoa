# DIN_Bridge USB Host 機能追加 — 実行計画

## 1. 目的

`UUT/DIN_Bridge` を **USB MIDI 1.0 → UMP → USB MIDI 2.0** のブリッジデバイスとして完成させる。

USB Host ポートに接続した **USB MIDI 1.0 デバイス**（キーボード、コントローラ、音源等）から受信した MIDI 通信を **UMP（Universal MIDI Packet）** に変換し、PC 側には既存の `tusb_ump` ドライバを通じて **USB MIDI 2.0 デバイス** として提供する。

### 1.1 最終製品像

本プロジェクトのゴールは、単なる Host 認識ではなく、以下の変換パイプラインを持つブリッジデバイスの完成である。

```
[USB MIDI 1.0 デバイス] ──USB Host──→ [RP2040 DIN_Bridge] ──USB Device (UMP)──→ [PC / DAW]
   (キーボード等)              │              ↑
                               │         MIDI 2.0 として認識
                               │    (Alternate Setting #1)
                               ↕
                        [DIN MIDI 5ピン]（既存機能、オプション経路）
```

**PC から見た動作:**

- DIN_Bridge 自身は **USB MIDI 2.0 対応デバイス**（`usb_descriptors.cpp` の UMP デスクリプタ）として列挙される
- Host ポートに接続した USB MIDI 1.0 デバイスの演奏データが、UMP に変換されて PC の DAW に届く
- PC から送信した UMP は、必要に応じて USB MIDI 1.0 形式に戻して Host 側デバイスへ転送できる（逆方向ブリッジ）

### 1.2 フェーズ全体像

| Phase | 名称 | ゴール |
|-------|------|--------|
| 0 | 前提確認 | ハードウェア・SDK の準備 |
| 1 | ビルド基盤 | Device + Host デュアルロールでビルド |
| 2 | デバイス認識 | USB Host で MIDI 1.0 デバイスを列挙 |
| 3 | **MIDI 1.0 → UMP 変換** | Host 受信データを UMP 化し PC へ送出（**コア機能**） |
| 4 | **MIDI 2.0 デバイスとしての動作確立** | PC が MIDI 2.0 として認識・UMP 送受信を検証 |
| 5 | 逆方向・DIN ブリッジ統合 | UMP → MIDI 1.0 / DIN 双方向ルーティング |
| 6 | 安定化・拡張 | エラー処理、複数デバイス、長期運用 |

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
- Host 側 D+/D- 用 GPIO のピンアサインをボード設計に合わせて決定

> **要確認:** ProtoZOA / DIN_Bridge ボードに Host 用 USB-A コネクタまたは PIO USB 用 GPIO が割り当てられているか、ハードウェア仕様書で確認すること。

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
│   ├── bridge_host_to_ump_device()   (Phase 3: コア変換)
│   ├── bridge_ump_device_to_host()   (Phase 5: 逆方向)
│   ├── bridge_usb_device_to_din()    (既存)
│   └── bridge_din_to_usb_device()    (既存)
└── コールバック
    ├── tuh_midi_mount_cb()
    ├── tuh_midi_unmount_cb()
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

### Phase 0: 前提確認（ハードウェア・SDK）

- [ ] ボードの Host 用 USB コネクタ / PIO USB GPIO ピンを確認
- [ ] Host 5V 電源制御 GPIO の有無を確認
- [ ] Pico SDK **2.1 以降** を使用（TinyUSB MIDI Host サポート）
- [ ] TinyUSB に `midi_host.h` が含まれることを確認
- [ ] CPU クロックを 120 MHz または 240 MHz に設定
- [ ] PC 側 DAW / MIDI 2.0 対応ツール（MIDI 2.0 動作確認用）を準備

**完了条件:** Host ポートに USB MIDI 1.0 デバイスを物理接続できる状態

---

### Phase 1: ビルド基盤 — Host スタック有効化

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
- `tinyusb_host`, `pico_pio_usb` をリンク

#### 5.1.3 ビルド確認

- [ ] Device + Host デュアルロールでビルド成功
- [ ] 既存の MIDI 2.0 デスクリプタ（`usb_descriptors.cpp`）がそのまま有効

**完了条件:** ビルド成功、PC への MIDI 2.0 デバイス列挙に影響なし

---

### Phase 2: USB MIDI 1.0 デバイス認識

Host ポートに接続した USB MIDI 1.0 デバイスを列挙・認識する。

#### 5.2.1 Host 初期化

```cpp
void usb_host_init(void) {
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = HOST_PIN_DP;  // ボードに合わせて設定
    tuh_configure(BOARD_HOST_RHPORT_NUM, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tusb_init();
}
```

#### 5.2.2 認識コールバック

```cpp
void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_data_t* mount_data) {
    // VID/PID, ケーブル数, エンドポイント情報を記録
    host_midi_mounted[idx] = true;
}

void tuh_midi_unmount_cb(uint8_t idx) {
    host_midi_mounted[idx] = false;
}

void tuh_midi_rx_cb(uint8_t idx, uint32_t xferred_bytes) {
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

**完了条件:** USB MIDI 1.0 デバイス接続時に `tuh_midi_mount_cb` が呼ばれ、VID/PID が確認できる

---

### Phase 3: USB MIDI 1.0 → UMP 変換（コア機能）

**本プロジェクトの最重要フェーズ。** Host ポートから受信した USB MIDI 1.0 データを UMP に変換し、内部バッファに蓄積する。

#### 5.3.1 受信・変換処理

```cpp
bytestreamToUMP host2ump;

void process_usb_host_midi_rx(uint8_t idx) {
    if (!tuh_midi_mounted(idx)) return;

    uint8_t packet[4];
    while (tuh_midi_read(idx, packet)) {
        // USB MIDI 1.0 パケットから MIDI バイトを抽出
        uint8_t cin  = packet[0] >> 4;
        uint8_t b0   = packet[1];
        uint8_t b1   = packet[2];
        uint8_t b2   = packet[3];

        // CIN に応じて有効バイト数を判定し bytestreamToUMP へ投入
        host2ump.bytestreamParse(b0);
        if (cin != 0x2 && cin != 0x6 && cin != 0xC) host2ump.bytestreamParse(b1);
        if (cin >= 0x8 && cin <= 0xE)               host2ump.bytestreamParse(b2);

        // Active Sensing (0xFE) はスキップ
    }
}

// 変換済み UMP を取り出す
bool pop_host_ump(uint32_t* ump) {
    if (!host2ump.availableUMP()) return false;
    *ump = host2ump.readUMP();
    return true;
}
```

#### 5.3.2 UMP バッファリング

- Host からの変換 UMP はリングバッファまたは `bytestreamToUMP` の出力キューで保持
- `tud_ump_n_mounted(0)` が true のときのみ PC へ送出（Phase 4）

#### 5.3.3 デバッグ確認（PC 送出前）

Phase 3 単体では `printf` で UMP をダンプし、変換正確性を確認:

```
Host MIDI IN: C4 Note On → UMP: 0x20904090 (MIDI 1.0 Channel Voice UMP)
```

**完了条件:** Host キーボードの演奏が正しい UMP ワードに変換される（シリアルログで検証）

---

### Phase 4: MIDI 2.0 デバイスとして PC へ送出

Phase 3 で生成した UMP を `tud_ump_write()` 経由で PC に送出し、**MIDI 2.0 デバイスとしての動作** を確立する。

#### 5.4.1 PC への UMP 送出

```cpp
void bridge_host_to_ump_device(void) {
    if (!tud_ump_n_mounted(0)) return;

    uint32_t ump;
    while (pop_host_ump(&ump)) {
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

    // Host MIDI 1.0 受信 → UMP 変換
    for (uint8_t idx = 0; idx < CFG_TUH_DEVICE_MAX; idx++) {
        if (host_midi_mounted[idx]) {
            process_usb_host_midi_rx(idx);
        }
    }

    // UMP → PC（MIDI 2.0 デバイスとして送出）
    bridge_host_to_ump_device();
}
```

#### 5.4.4 動作確認ポイント

| 確認項目 | 期待結果 |
|----------|----------|
| PC が DIN_Bridge を MIDI 2.0 デバイスとして認識 | Alternate Setting #1 が選択可能 |
| Host キーボード演奏 → PC DAW | DAW の MIDI 2.0 入力にノートイベント表示 |
| UMP フォーマット | MIDI 1.0 Channel Voice UMP（Type 0x2）として正しく届く |
| タイミング | 演奏遅延が実用範囲内（目安: < 10 ms） |

**完了条件:** Host ポートの USB MIDI 1.0 デバイスの演奏が、PC 上の MIDI 2.0 対応 DAW に UMP として届く

---

### Phase 5: 逆方向ブリッジ・DIN 統合

PC（MIDI 2.0）および DIN ポートからのデータを Host 側 USB MIDI 1.0 デバイスへ転送する。

#### 5.5.1 PC → Host（UMP → USB MIDI 1.0）

```cpp
umpToBytestream device2host;

void bridge_ump_device_to_host(uint8_t idx) {
    if (!tuh_midi_mounted(idx)) return;

    uint32_t UMPpacket[4];
    uint32_t umpCount = tud_ump_read(0, UMPpacket, 4);
    for (uint8_t i = 0; i < umpCount; i++) {
        device2host.UMPStreamParse(UMPpacket[i]);
        while (device2host.availableBS()) {
            uint8_t byte = device2host.readBS();
            tuh_midi_stream_write(idx, 0, &byte, 1);
        }
    }
    tuh_midi_write_flush(idx);
}
```

#### 5.5.2 DIN ポート統合（既存機能との共存）

| 経路 | 動作 |
|------|------|
| Host → DIN | UMP 変換後、`uart_tx_program_putc` で DIN OUT |
| DIN → PC | 既存: DIN RX → `tud_ump_write` |
| DIN → Host | DIN RX → UMP → `umpToBytestream` → Host OUT |
| PC → DIN | 既存: `tud_ump_read` → DIN TX |

#### 5.5.3 ルーティング方針

| シナリオ | デフォルト |
|----------|------------|
| Host MIDI IN → PC（MIDI 2.0 UMP） | **有効（Phase 4 で完成）** |
| Host MIDI IN → DIN OUT | 有効 |
| PC MIDI OUT → Host MIDI OUT | 有効 |
| DIN IN → PC | 有効（既存） |
| DIN IN → Host MIDI OUT | 有効 |

**完了条件:** 全経路でノート ON/OFF・コントロールチェンジが双方向に正しく伝搬する

---

### Phase 6: 安定化・拡張

- [ ] Active Sensing (0xFE) フィルタリング
- [ ] SysEx 7/8 バイトストリームの長文対応（`bytestreamToUMP` の Sysex 状態機械）
- [ ] 複数 USB MIDI 1.0 デバイス対応（`idx` ごとの Group 割り当て）
- [ ] Host デバイス切断時の UMP バッファフラッシュ
- [ ] 接続状態 LED / ステータス表示
- [ ] MIDI 2.0 拡張メッセージ（NRPN/RPN 等）の変換精度検証
- [ ] 連続演奏ストレステスト（アルペジオ、高速 CC 等）

**完了条件:** 長時間運用・複数デバイス・SysEx を含む実用シナリオで安定動作

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

### 7.4 Phase 5（双方向・DIN テスト）

| # | 手順 | 期待結果 |
|---|------|----------|
| 1 | PC DAW からノート ON → Host 接続音源 | 音源が鳴る |
| 2 | Host 演奏 → DIN OUT | DIN モニタに MIDI 1.0 バイト列 |
| 3 | DIN IN → PC | DAW に UMP として記録 |
| 4 | DIN IN → Host 音源 | 音源が鳴る |
| 5 | 連続演奏（アルペジオ） | ノート取りこぼしなし |

### 7.5 使用ツール

- シリアルモニタ（UMP ダンプ確認）
- MIDI 2.0 対応 DAW（Ableton Live 12+, Cubase 14+ 等）または UMP モニタ
- USB MIDI 1.0 テストデバイス（キーボード、Korg microKEY 等）
- DIN MIDI モニタ（Phase 5）

---

## 8. 依存関係・リスク

| リスク | 影響 | 対策 |
|--------|------|------|
| Pico SDK / TinyUSB バージョン不足 | `tuh_midi_*` API 不在 | SDK 2.1+、必要なら TinyUSB 更新 |
| PIO USB ピン未配線 | Host ポート不動作 | Phase 0 でハードウェア確認 |
| CPU クロック 120 MHz 以外 | PIO USB 不安定 | `set_sys_clock_khz(120000, true)` |
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
| Phase 5 | 逆方向・DIN ブリッジ統合 | 1〜2 日 |
| Phase 6 | 安定化・テスト | 1 日 |
| **合計** | | **5.5〜7.5 日** |

---

## 10. 参考資料

- [TinyUSB MIDI Host API (`midi_host.h`)](https://github.com/hathach/tinyusb/tree/master/src/class/midi)
- [rppicomidi/usb_midi_host — RP2040 Host 例・API 解説](https://github.com/rppicomidi/usb_midi_host)
- [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)
- [Adafruit USB MIDI Host Messenger ガイド](https://learn.adafruit.com/usb-midi-host-messenger)
- 本リポジトリ `lib/tusb_ump/README.md`（Device 側 UMP ドライバ、MIDI 1.0 ↔ UMP 変換）
- 本リポジトリ `lib/AM_MIDI2.0Lib/docs/bytestreamToUMP.md`（バイトストリーム → UMP 変換仕様）
- [MIDI 2.0 UMP 仕様](https://midi.org/midi-2-0-specification)

---

## 11. 次のアクション

1. **Phase 0:** ボードの Host USB / PIO USB ピン配置を確認
2. **Phase 1:** `UUT/DIN_Bridge/tusb_config.h` 作成と `CMakeLists.txt` 更新
3. **Phase 2:** `usb_host_midi.cpp` に mount/unmount コールバックを実装し、認識テスト
4. **Phase 3:** `bytestreamToUMP` による Host MIDI 1.0 → UMP 変換を実装
5. **Phase 4:** `tud_ump_write()` で PC へ UMP 送出、MIDI 2.0 デバイス動作を確認

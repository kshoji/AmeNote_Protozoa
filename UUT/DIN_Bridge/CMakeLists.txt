
file(GLOB common_SRC CONFIGURE_DEPENDS
        "../../Common/*.cpp"
        "../../lib/tusb_ump/*.cpp"
        "../../lib/AM_MIDI2.0Lib/src/*.cpp"
        )

add_executable(UUT_DIN_BRIDGE
        main.cpp
        usb_descriptors.cpp
        usb_host_midi.cpp
        ${common_SRC}
        )

# DIN_Bridge ローカルの tusb_config.h を Common より優先
target_include_directories(UUT_DIN_BRIDGE PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
        .
        ../../Common
        ../../Common/include
        ../../lib/tusb_ump
        ../../lib/AM_MIDI2.0Lib
        )

pico_generate_pio_header(UUT_DIN_BRIDGE ${CMAKE_CURRENT_LIST_DIR}/../../Common/pio_serial/uart_rx.pio)
pico_generate_pio_header(UUT_DIN_BRIDGE ${CMAKE_CURRENT_LIST_DIR}/../../Common/pio_serial/uart_tx.pio)

# Temporary suppression of compile warning for MIDI2_CPP
target_compile_options(UUT_DIN_BRIDGE PRIVATE -Wno-narrowing -Wno-unused-variable)

# Phase 0 確定: PIO USB Host D+ = GP20（D- = GP21）、VBUS EN = GP22
target_compile_definitions(UUT_DIN_BRIDGE PRIVATE
        HOST_PIN_DP=20
        HOST_PIN_VBUSEN=22
        HOST_PIN_VBUSEN_STATE=1
        )

set(DIN_BRIDGE_LIBS
        pico_stdlib
        pico_stdio
        pico_unique_id
        pico_multicore
        tinyusb_device
        tinyusb_host
        hardware_pio
        hardware_clocks
        hardware_adc
        hardware_spi
        hardware_flash
        hardware_dma
        libmidi2
)

# Pico-PIO-USB（RHPort1 Host）— SDK / TinyUSB がターゲットを生成済みの場合のみリンク
if (TARGET tinyusb_pico_pio_usb)
        list(APPEND DIN_BRIDGE_LIBS tinyusb_pico_pio_usb)
else()
        message(WARNING "tinyusb_pico_pio_usb not found. Set PICO_PIO_USB_PATH to lib/Pico-PIO-USB before pico_sdk_init().")
endif()

target_link_libraries(UUT_DIN_BRIDGE ${DIN_BRIDGE_LIBS})

# Phase 2: mount/unmount 認識ログを USB CDC シリアルへ出力
pico_enable_stdio_usb(UUT_DIN_BRIDGE 1)
pico_enable_stdio_uart(UUT_DIN_BRIDGE 0)

# create map/bin/hex file etc.
pico_add_extra_outputs(UUT_DIN_BRIDGE)

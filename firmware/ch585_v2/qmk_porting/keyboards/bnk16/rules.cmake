# BNK16 hardware and base settings. Feature switches are in keymaps/vial/rules.cmake.
set(MOUSE_ENABLE ON CACHE BOOL "KB" FORCE)
set(NKRO_ENABLE ON CACHE BOOL "KB" FORCE)
set(ENCODER_ENABLE ON CACHE BOOL "KB" FORCE)
set(ENCODER_MAP_ENABLE ON CACHE BOOL "KB" FORCE)
set(VIA_ENABLE ON CACHE BOOL "KB" FORCE)
set(VIAL_ENABLE ON CACHE BOOL "KB" FORCE)
# Off: the UF2 bootloader checks the top-left key itself. QMK's bootmagic would also wipe the stored
# settings, and it could fire when the board is woken from shutdown with that key.
set(BOOTMAGIC_ENABLE OFF CACHE BOOL "KB" FORCE)

# The firmware runs behind the UF2 bootloader (firmware/ch585_v2/bootloader, layout in layout.cmake)
set(UF2_BOOTLOADER ON CACHE BOOL "KB" FORCE)
set(GRAVE_ESC_ENABLE OFF CACHE BOOL "KB" FORCE)
set(COMMAND_ENABLE OFF CACHE BOOL "KB" FORCE)
set(MACRO_ENABLE ON CACHE BOOL "KB" FORCE)

set(RGBLIGHT_ENABLE OFF CACHE BOOL "KB" FORCE)
set(AUXILIARY_RGB_ENABLE OFF CACHE BOOL "KB" FORCE)
set(OLED_ENABLE OFF CACHE BOOL "KB" FORCE)

set(SPLIT_KEYBOARD OFF CACHE BOOL "KB" FORCE)
set(EEPROM_DRIVER "custom" CACHE STRING "KB" FORCE)

set(USB_ENABLE ON CACHE BOOL "KB" FORCE)
set(ESB_ROLE "keyboard" CACHE STRING "KB")

# RGB: 16 key LEDs + 1 status LED on PA10 (WS2812 via TMR1 PWM + DMA)
set(RGB_MATRIX_ENABLE ON CACHE BOOL "KB" FORCE)
set(WS2812_DRIVER "pwm" CACHE STRING "KB" FORCE)
set(VIALRGB_ENABLE ON CACHE BOOL "KB" FORCE)

# USB connector is on the HS port; the FS port would claim PB10/PB11 (layer and mode switch)
add_definitions(-DUSB_PORT_CONFIG=USB_PORT_HIGH_SPEED)

# Up to 3 key reports per BLE connection event (the SDK default is 1)
add_definitions(-DBLE_TX_NUM_EVENT=3)

# Test build only (BNK16_TEST_TIMERS=1 for build-bnk16.sh): short idle stages instead of 5/5/20 min.
# Sleep 15 s after the last key, slow BLE interval 30 s later, shutdown 90 s after the last key.
if(BNK16_TEST_TIMERS)
    message(STATUS "BNK16: test timers (15/30/90 s)")
    add_definitions(-DPOWER_SLEEP_TIMEOUT_MS=15000 -DBLE_CONN_INTERVAL_L1_TIME=30000 -DPOWER_SHUTDOWN_TIMEOUT_MS=90000)
endif()

# Test variants only (BNK16_DEFS="-D..." for build-bnk16.sh), e.g. -DBNK16_BOOT_DIAG, -DBNK16_DEEP_SLEEP_NO_CHIP_SLEEP,
# -DHAL_SLEEP_XT_PRE=0, -DHAL_BLE_IRQ_PRIORITY
if(BNK16_DEFS)
    message(STATUS "BNK16: extra defines ${BNK16_DEFS}")
    separate_arguments(bnk16_defs UNIX_COMMAND "${BNK16_DEFS}")
    add_definitions(${bnk16_defs})
endif()

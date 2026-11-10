# BNK16 test B (see main.c): WCH's official HID_Keyboard example with the official BLE library and SDK,
# all taken from evt/ (fetch-evt.sh) except main.c, hidkbd.c and haptic.c. Linked at the firmware start behind the
# UF2 bootloader. Built by build-bletest.sh. Nothing from our own sdk/ or QMK is used.

cmake_minimum_required(VERSION 3.20)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED TOOLCHAIN_PREFIX)
    set(TOOLCHAIN_PREFIX riscv-wch-elf)
endif()
find_program(CMAKE_C_COMPILER NAMES ${TOOLCHAIN_PREFIX}-gcc PATHS ${ADDITIONAL_TOOLCHAIN_PATH} REQUIRED)
set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})
find_program(CMAKE_OBJCOPY NAMES ${TOOLCHAIN_PREFIX}-objcopy PATHS ${ADDITIONAL_TOOLCHAIN_PATH} REQUIRED)

project(bnk16_bletest C ASM)

include(${CMAKE_CURRENT_LIST_DIR}/../../bootloader/layout.cmake)

# EVT_DIR: the GitHub EVT (evt/, default) or a copy of the newer WCH website EVT (evt-web/, test B4)
if(NOT EVT_DIR)
    set(EVT_DIR ${CMAKE_CURRENT_LIST_DIR}/evt)
endif()
if(NOT BLETEST_VARIANTS)
    set(BLETEST_VARIANTS b1-nosleep-100ms:0:80:96 b2-sleep-15ms:1:12:12 b3-sleep-100ms:1:80:96)
endif()
set(EXAM ${EVT_DIR}/EVT/EXAM)
# BLETEST_REGDUMP=1: type a register snapshot as the second report (phase 47e)
if(BLETEST_REGDUMP)
    set(BLETEST_REGDUMP_ON 1)
else()
    set(BLETEST_REGDUMP_ON 0)
endif()
set(SRC ${EXAM}/SRC)
set(BLE ${EXAM}/BLE)
set(KBD ${BLE}/HID_Keyboard)
if(NOT EXISTS ${BLE}/LIB/libCH58xBLE.a)
    message(FATAL_ERROR "WCH EVT missing: run tests/bletest/fetch-evt.sh first")
endif()

# official linker script, FLASH moved behind our bootloader
file(READ ${SRC}/Ld/Link.ld ld)
string(REPLACE "FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 448K"
    "FLASH (rx) : ORIGIN = ${BNK16_APP_START}, LENGTH = ${BNK16_APP_SIZE}" ld_app "${ld}")
if(ld_app STREQUAL ld)
    message(FATAL_ERROR "bletest: FLASH line not found in the EVT Link.ld")
endif()
set(LINKER_SCRIPT ${PROJECT_BINARY_DIR}/bletest.ld)
file(WRITE ${LINKER_SCRIPT} "${ld_app}")

# SLEEP.c waits for UART0 to drain before each sleep when DEBUG is undefined (DEBUG == Debug_UART0 == 0
# then); this build has no UART, so that wait is taken out
file(READ ${BLE}/HAL/SLEEP.c sleep_src)
string(REPLACE "#if(DEBUG == Debug_UART0)" "#if 0 /* BNK16: no UART */" sleep_patched "${sleep_src}")
if(sleep_patched STREQUAL sleep_src)
    message(FATAL_ERROR "bletest: UART0 wait not found in the EVT SLEEP.c")
endif()
set(SLEEP_C ${PROJECT_BINARY_DIR}/SLEEP.c)
file(WRITE ${SLEEP_C} "${sleep_patched}")

file(GLOB STDPERIPH ${SRC}/StdPeriphDriver/*.c)
file(GLOB PROFILE ${KBD}/Profile/*.c)

# name:sleep:min_interval:max_interval (units of 1.25 ms)
#   b1: control without chip sleep (WFI only, like our firmware today), 100-120 ms
#   b2: sleep, 15 ms (our firmware's interval while awake)
#   b3: sleep, 100-120 ms (our firmware's interval after 10 min idle)
set(VARIANTS ${BLETEST_VARIANTS})

foreach(entry ${VARIANTS})
    string(REPLACE ":" ";" f ${entry})
    list(GET f 0 v)
    list(GET f 1 sleep)
    list(GET f 2 imin)
    list(GET f 3 imax)
    set(t bletest-${v})
    add_executable(${t}.elf
        main.c
        hidkbd.c
        haptic.c
        ${PROFILE}
        ${BLE}/HAL/MCU.c
        ${BLE}/HAL/RTC.c
        ${SLEEP_C}
        ${BLE}/LIB/ble_task_scheduler.S
        ${STDPERIPH}
        ${SRC}/Startup/startup_CH585.S
    )
    target_include_directories(${t}.elf PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../common
        ${KBD}/APP/include
        ${KBD}/Profile/include
        ${BLE}/HAL/include
        ${BLE}/LIB
        ${SRC}/StdPeriphDriver/inc
        ${SRC}/RVMSIS
    )
    target_compile_definitions(${t}.elf PRIVATE
        HAL_SLEEP=${sleep}
        BLE_MAC=1
        BLE_SNV_ADDR=0x6000 # data flash block 0x6000-0x6FFF is unused; ours: QMK 0x4F00-0x57FF, bonds 0x7000
        BLETEST_MIN_INTERVAL=${imin}
        BLETEST_MAX_INTERVAL=${imax}
        BLETEST_REPORT_MS=30000
        BLETEST_REGDUMP=${BLETEST_REGDUMP_ON}
    )
    target_compile_options(${t}.elf PRIVATE
        -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore
        -Os -std=gnu99 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common
        -Wall -Wno-comment -Wno-unused-variable -Wno-unused-function
    )
    target_link_options(${t}.elf PRIVATE
        -march=rv32imac -mabi=ilp32
        -T ${LINKER_SCRIPT}
        -nostartfiles
        -Xlinker --gc-sections
        -Wl,--print-memory-usage
        -Wl,-Map,${t}.map
        --specs=nano.specs
        --specs=nosys.specs
    )
    target_link_libraries(${t}.elf PRIVATE ${BLE}/LIB/libCH58xBLE.a ${SRC}/StdPeriphDriver/libISP585.a)
    add_custom_command(TARGET ${t}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:${t}.elf> ${PROJECT_BINARY_DIR}/${t}.bin
    )
endforeach()

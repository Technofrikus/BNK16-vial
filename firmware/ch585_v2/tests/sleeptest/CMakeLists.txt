# BNK16 sleep test (test A, see main.c): one small firmware per sleep variant, linked at the firmware
# start behind the UF2 bootloader. Built by build-sleeptest.sh. No QMK, no BLE.

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

project(bnk16_sleeptest C ASM)

include(${CMAKE_CURRENT_LIST_DIR}/../../bootloader/layout.cmake)
set(SDK ${CMAKE_CURRENT_LIST_DIR}/../../sdk)

set(LINKER_SCRIPT ${PROJECT_BINARY_DIR}/sleeptest.ld)
bnk16_make_linker_script(${LINKER_SCRIPT} ${BNK16_APP_START} ${BNK16_APP_SIZE})

# name:mode:period_s:pins (mode = SLEEPTEST_MODE in main.c). The first set checks that each sleep works
# (5 s); the "m" set measures the current: 60 s sleeps, pins as in the shutdown, plus the shutdown itself.
set(VARIANTS
    1-idle:1:5:0 2-sleep:2:5:0 3-sleep-noext:3:5:0 4-sleep-wfe:4:5:0 5-sleep-wfe-sev:5:5:0
    6-halt:6:5:0 7-halt-sev:7:5:0
    m2-sleep:2:60:1 m4-sleep-wfe:4:60:1 m8-shutdown:8:60:1)

foreach(entry ${VARIANTS})
    string(REPLACE ":" ";" f ${entry})
    list(GET f 0 v)
    list(GET f 1 mode)
    list(GET f 2 period)
    list(GET f 3 pins)
    set(t sleeptest-${v})
    add_executable(${t}.elf
        main.c
        ${SDK}/StdPeriphDriver/CH58x_sys.c
        ${SDK}/StdPeriphDriver/CH58x_clk.c
        ${SDK}/StdPeriphDriver/CH58x_pwr.c
        ${SDK}/StdPeriphDriver/CH58x_gpio.c
        ${SDK}/Startup/startup_CH585.S
    )
    target_include_directories(${t}.elf PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../common ${SDK}/StdPeriphDriver/inc ${SDK}/RVMSIS)
    target_compile_definitions(${t}.elf PRIVATE SLEEPTEST_MODE=${mode} SLEEPTEST_PERIOD_S=${period} SLEEPTEST_PINS=${pins})
    target_compile_options(${t}.elf PRIVATE
        -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore
        -Os -std=gnu99 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common
        -Wall -Wno-comment
    )
    target_link_options(${t}.elf PRIVATE
        -march=rv32imac -mabi=ilp32
        -T ${LINKER_SCRIPT}
        -nostartfiles
        -Xlinker --gc-sections
        -Wl,-Map,${t}.map
        --specs=nano.specs
        --specs=nosys.specs
    )
    target_link_libraries(${t}.elf PRIVATE ${SDK}/StdPeriphDriver/libISP585.a)
    add_custom_command(TARGET ${t}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:${t}.elf> ${PROJECT_BINARY_DIR}/${t}.bin
    )
endforeach()

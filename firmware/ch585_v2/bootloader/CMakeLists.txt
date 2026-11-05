# BNK16 UF2 bootloader: a standalone CMake project, built by build-bnk16.sh before the firmware.
# Uses only a few files of the WCH SDK in ../sdk, no QMK.

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
find_program(CMAKE_SIZE NAMES ${TOOLCHAIN_PREFIX}-size PATHS ${ADDITIONAL_TOOLCHAIN_PATH})

project(bnk16_bootloader C ASM)

include(${CMAKE_CURRENT_LIST_DIR}/layout.cmake)

set(SDK ${CMAKE_CURRENT_LIST_DIR}/../sdk)

execute_process(COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE BL_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT BL_VERSION)
    set(BL_VERSION "unknown")
endif()

set(LINKER_SCRIPT ${PROJECT_BINARY_DIR}/bootloader.ld)
bnk16_make_linker_script(${LINKER_SCRIPT} 0x00000000 ${BNK16_BOOTLOADER_SIZE})

add_executable(bnk16_bootloader.elf
    main.c
    usb_msc.c
    ghostfat.c
    ${SDK}/StdPeriphDriver/CH58x_sys.c
    ${SDK}/StdPeriphDriver/CH58x_clk.c
    ${SDK}/Startup/startup_CH585.S
)

target_include_directories(bnk16_bootloader.elf PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${SDK}/StdPeriphDriver/inc
    ${SDK}/RVMSIS
)

target_compile_definitions(bnk16_bootloader.elf PRIVATE
    BL_APP_START=${BNK16_APP_START}
    BL_FLASH_SIZE=${BNK16_FLASH_SIZE}
    BL_UF2_FAMILY=${BNK16_UF2_FAMILY}
    BL_VERSION="${BL_VERSION}"
)

target_compile_options(bnk16_bootloader.elf PRIVATE
    -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore
    -Os -std=gnu99 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common
    -Wall -Wno-comment
)

target_link_options(bnk16_bootloader.elf PRIVATE
    -march=rv32imac -mabi=ilp32
    -T ${LINKER_SCRIPT}
    -nostartfiles
    -Xlinker --gc-sections
    -Wl,--print-memory-usage
    -Wl,-Map,bnk16_bootloader.map
    --specs=nano.specs
    --specs=nosys.specs
)

target_link_libraries(bnk16_bootloader.elf PRIVATE ${SDK}/StdPeriphDriver/libISP585.a)

add_custom_command(TARGET bnk16_bootloader.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:bnk16_bootloader.elf> ${PROJECT_BINARY_DIR}/bnk16_bootloader.bin
    COMMAND ${CMAKE_OBJCOPY} -Oihex $<TARGET_FILE:bnk16_bootloader.elf> ${PROJECT_BINARY_DIR}/bnk16_bootloader.hex
)

# Flash layout shared by the bootloader and the BNK16 firmware. Change it only together with a full
# wchisp flash: a firmware built for one layout does not boot from a bootloader built for another.
#
#   0x00000 - 0x07FFF   bootloader (32 KB)
#   0x08000 - 0x6FFFF   firmware (416 KB)
#
# QMK settings and BLE bonds live in the separate 32 KB data flash and are not touched by updates.

set(BNK16_FLASH_SIZE      0x70000)   # 448 KB code flash
set(BNK16_BOOTLOADER_SIZE 0x08000)
set(BNK16_APP_START       0x08000)
math(EXPR BNK16_APP_SIZE "${BNK16_FLASH_SIZE} - ${BNK16_APP_START}" OUTPUT_FORMAT HEXADECIMAL)

# Our own UF2 family ID. Deliberately not Binepad's 0x1aa143c7, so a stock UF2 (built for a
# different PCB revision and a different layout) is ignored instead of being flashed.
set(BNK16_UF2_FAMILY 0x6B16C585)

# Write a copy of the SDK linker script with the FLASH region moved to the given origin and length.
function(bnk16_make_linker_script out origin length)
    file(READ ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../sdk/Ld/Link_APP.ld ld)
    string(REGEX REPLACE
        "FLASH \\(rx\\) : ORIGIN = 0x00000000, LENGTH = 448K"
        "FLASH (rx) : ORIGIN = ${origin}, LENGTH = ${length}"
        patched "${ld}")
    if(patched STREQUAL ld)
        message(FATAL_ERROR "bnk16_make_linker_script: FLASH line not found in Link_APP.ld")
    endif()
    file(WRITE ${out} "${patched}")
endfunction()

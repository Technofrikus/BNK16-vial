# Copyright 2022 Huckies <https://github.com/Huckies>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.20)

# Debug message
message("Entering ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt")

add_library(CH582_APP INTERFACE)
add_library(CH582_IAP INTERFACE)

# BLE library and its headers; a test build can point this at another copy (BNK16_BLE_LIB_DIR in build-bnk16.sh)
if(NOT BNK16_BLE_LIB_DIR)
    set(BNK16_BLE_LIB_DIR ${CMAKE_CURRENT_LIST_DIR}/BLE_LIB)
endif()
message(STATUS "BLE library: ${BNK16_BLE_LIB_DIR}")

target_include_directories(CH582_APP INTERFACE
    ${BNK16_BLE_LIB_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/HAL/include
    ${CMAKE_CURRENT_LIST_DIR}/LWNS
    ${CMAKE_CURRENT_LIST_DIR}/printf
    ${CMAKE_CURRENT_LIST_DIR}/RVMSIS
    ${CMAKE_CURRENT_LIST_DIR}/StdPeriphDriver/inc

    # ${CMAKE_CURRENT_LIST_DIR}/USB_LIB
)
target_include_directories(CH582_IAP INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/BLE_LIB
    ${CMAKE_CURRENT_LIST_DIR}/HAL/include
    ${CMAKE_CURRENT_LIST_DIR}/LWNS
    ${CMAKE_CURRENT_LIST_DIR}/printf
    ${CMAKE_CURRENT_LIST_DIR}/RVMSIS
    ${CMAKE_CURRENT_LIST_DIR}/StdPeriphDriver/inc

    # ${CMAKE_CURRENT_LIST_DIR}/USB_LIB
)

file(GLOB_RECURSE ch582_sdk_temp
    "${CMAKE_CURRENT_LIST_DIR}/printf/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/RVMSIS/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/StdPeriphDriver/*.c"

    # "${CMAKE_CURRENT_LIST_DIR}/USB_LIB/*.c"
)
# 移除USB主机模式驱动（我们只用USB设备模式，且CHRV3UFI.H大小写不匹配会导致Linux编译失败）
list(REMOVE_ITEM ch582_sdk_temp
    "${CMAKE_CURRENT_LIST_DIR}/StdPeriphDriver/CH58x_usbhostBase.c"
    "${CMAKE_CURRENT_LIST_DIR}/StdPeriphDriver/CH58x_usbhostClass.c"
)

if(BUILD_WIRELESS_LIB OR(NOT BLE_ENABLE AND NOT ESB_ENABLE))
    list(APPEND ch582_sdk_temp
        "${CMAKE_CURRENT_LIST_DIR}/HAL/MCU.c"
        "${CMAKE_CURRENT_LIST_DIR}/HAL/RTC.c"
        "${CMAKE_CURRENT_LIST_DIR}/HAL/SLEEP.c"
    )
    target_sources(CH582_APP INTERFACE
        ${ch582_sdk_temp}
        "${CMAKE_CURRENT_LIST_DIR}/Startup/startup_CH585.S"
    )
endif()

target_sources(CH582_IAP INTERFACE
    ${ch582_sdk_temp}
    "${CMAKE_CURRENT_LIST_DIR}/Startup/startup_CH585.S"
)

if(UF2_BOOTLOADER)
    # Firmware linked behind the UF2 bootloader: same script, FLASH region moved
    include(${CMAKE_CURRENT_LIST_DIR}/../bootloader/layout.cmake)
    set(APP_LINKER_SCRIPT ${PROJECT_BINARY_DIR}/Link_APP_uf2.ld)
    bnk16_make_linker_script(${APP_LINKER_SCRIPT} ${BNK16_APP_START} ${BNK16_APP_SIZE})
    message(STATUS "UF2 bootloader layout: firmware at ${BNK16_APP_START}, ${BNK16_APP_SIZE} bytes")
else()
    set(APP_LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/Ld/Link_APP.ld)
endif()

target_link_options(CH582_APP INTERFACE
    -T ${APP_LINKER_SCRIPT}
)
target_link_options(CH582_IAP INTERFACE
    -T ${CMAKE_CURRENT_LIST_DIR}/Ld/Link_IAP.ld
)

target_link_libraries(CH582_APP INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/StdPeriphDriver/libISP585.a
    -Wl,--whole-archive
    ${BNK16_BLE_LIB_DIR}/libCH58xBLE.a
    -Wl,--no-whole-archive

    # ${CMAKE_CURRENT_LIST_DIR}/USB_LIB/libRV3UFI.a
)
target_link_libraries(CH582_IAP INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/StdPeriphDriver/libISP585.a
    -Wl,--whole-archive
    ${CMAKE_CURRENT_LIST_DIR}/BLE_LIB/libCH58xBLE.a
    -Wl,--no-whole-archive

    # ${CMAKE_CURRENT_LIST_DIR}/USB_LIB/libRV3UFI.a
)

# Debug message
message("Exiting ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt")

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
idf_component_register(
    SRCS
        main.c
        hal_esp32s3.c
        ../../../common/core.c
        ../../../common/control.c
        ../../../common/usb_descriptors.c
    INCLUDE_DIRS
        .
        ../../../common
        ../../../../proto
    REQUIRES
        tinyusb_core driver esp_timer usb
)

if(DEFINED MIMHID_UART_TX_PIN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MIMHID_UART_TX_PIN=${MIMHID_UART_TX_PIN})
endif()
if(DEFINED MIMHID_UART_RX_PIN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MIMHID_UART_RX_PIN=${MIMHID_UART_RX_PIN})
endif()

# Control transfers of the target are taken where the stack receives them (see common/control.h).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=dcd_event_handler")

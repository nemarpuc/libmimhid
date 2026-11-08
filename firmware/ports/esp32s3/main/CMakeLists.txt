# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
idf_component_register(
    SRCS
        main.c
        hal_esp32s3.c
        ../../../common/core.c
        ../../../common/control.c
        ../../../common/ep0.c
        ../../../common/usb_descriptors.c
    INCLUDE_DIRS
        .
        ../../../common
        ../../../../proto
    REQUIRES
        tinyusb_core driver esp_timer usb
)

# The wiring is the board's: no pin is assumed.
if(NOT DEFINED MIMHID_UART_TX_PIN)
    message(FATAL_ERROR "Set -DMIMHID_UART_TX_PIN=<gpio>: the build assumes no pin")
endif()
if(NOT DEFINED MIMHID_UART_RX_PIN)
    message(FATAL_ERROR "Set -DMIMHID_UART_RX_PIN=<gpio>: the build assumes no pin")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MIMHID_UART_TX_PIN=${MIMHID_UART_TX_PIN}
    MIMHID_UART_RX_PIN=${MIMHID_UART_RX_PIN})

# Control transfers of the target are taken where the stack receives them (see common/control.h).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=dcd_event_handler" "-Wl,--wrap=dcd_edpt_stall" "-Wl,--wrap=dcd_edpt_xfer")

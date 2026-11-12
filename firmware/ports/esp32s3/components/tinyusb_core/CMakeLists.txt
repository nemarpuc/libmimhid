# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
# TinyUSB device stack compiled straight from the checkout in $TINYUSB_PATH, with this
# project's own descriptors and tusb_config.h (esp_tinyusb would supply its own).
set(tusb_src "$ENV{TINYUSB_PATH}/src")
set(common_dir "${CMAKE_CURRENT_LIST_DIR}/../../../../common")

idf_component_register(
    SRCS
        ${tusb_src}/tusb.c
        ${tusb_src}/common/tusb_fifo.c
        ${tusb_src}/device/usbd.c
        ${tusb_src}/device/usbd_control.c
        ${tusb_src}/portable/synopsys/dwc2/dcd_dwc2.c
        ${tusb_src}/portable/synopsys/dwc2/dwc2_common.c
    INCLUDE_DIRS
        ${tusb_src}
        ${common_dir}
        ${common_dir}/../../proto
    PRIV_REQUIRES
        esp_mm
)

# TinyUSB's ESP32-S3 controller table has ep_count 7 and ep_in_count 5, both with endpoint 0: the
# endpoint numbers are 1 to 6 and four IN endpoints can be open (see core.h). Full speed only (the
# built-in PHY).
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CFG_TUSB_MCU=OPT_MCU_ESP32S3
    CFG_TUSB_OS=OPT_OS_NONE
    "CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE|OPT_MODE_FULL_SPEED)"
    CFG_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED
    MIMUSB_MAX_ENDPOINT_NUMBER=6
    MIMUSB_MAX_ENDPOINT_BYTES=64
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format)

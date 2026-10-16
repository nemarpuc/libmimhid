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
        ${tusb_src}/class/hid/hid_device.c
        ${tusb_src}/portable/synopsys/dwc2/dcd_dwc2.c
        ${tusb_src}/portable/synopsys/dwc2/dwc2_common.c
    INCLUDE_DIRS
        ${tusb_src}
        ${common_dir}
        ${common_dir}/../../proto
    PRIV_REQUIRES
        esp_mm
)

# Four IN endpoints are usable on the ESP32-S3, so the host packs nodes into four interfaces.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CFG_TUSB_MCU=OPT_MCU_ESP32S3
    BOARD_TUD_RHPORT=0
    BOARD_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED
    MIMHID_HID_ITFS=4
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format)

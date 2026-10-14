idf_component_register(
    SRC_DIRS "." "audio" "clock" "net" "usb" "aoip" "sys"
    INCLUDE_DIRS "."
    LDFRAGMENTS "linker.lf"
    REQUIRES esp_eth esp_netif esp_timer nvs_flash driver esp_driver_gptimer
             esp_hw_support esp_system lwip log freertos esp_app_format
             espressif__tinyusb
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)

# usb/: TinyUSB is configured by our tusb_config.h (usb/tusb/), which the
# tinyusb component and main both need on their include path.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/usb/tusb")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/usb/tusb")

# usb/: the DWC2 interrupt must be IRAM-safe (survive flash writes). TinyUSB's
# dwc2_esp32.h allocates it with ESP_INTR_FLAG_LOWMED only; usb/uac2.c wraps
# esp_intr_alloc() and adds ESP_INTR_FLAG_IRAM for the USB OTG HS source.
# linker.lf moves the TinyUSB code on that path into IRAM.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_intr_alloc")

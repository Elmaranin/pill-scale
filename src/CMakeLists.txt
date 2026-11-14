# Firmware component. The ADC driver is one *_cell.cpp behind the LoadCell
# interface (include/load_cell.h); another ADC means another driver file, not
# an if in the logic.
set(srcs main.cpp cmd.cpp scale.cpp logbook.cpp web.cpp
         board/usb_console.cpp board/flash_partition.cpp board/wifi.cpp
         board/led.cpp board/buzzer.cpp board/hx711_cell.cpp)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS . ../include
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_usb_serial_jtag
        esp_partition
        esp_wifi
        esp_netif
        lwip
        esp_event
        esp_http_server
        esp_app_format
        esp_timer
        nvs_flash
)

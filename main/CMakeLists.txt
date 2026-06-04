set(req_components esp_event esp_netif esp_timer lwip nvs_flash usb mdns
                   harness_io harness_scpi harness_dut harness_io_expander)

set(srcs
    "main.c"
    "discovery_service.c"
    "network_init.c"
    "usb_backend.c"
    "log_server.c"
    "usbip_server.c"
    "http_server.c"
    "led_strip_encoder.c"
    "neopixel.c"
)

if(CONFIG_USBIP_VIRTUAL_LOGIC_ANALYZER OR CONFIG_USBIP_VIRTUAL_HARNESS)
    list(APPEND srcs
        "virtual_device.c"
        "virtual_cdc.c"
    )
endif()

if(CONFIG_USBIP_VIRTUAL_LOGIC_ANALYZER)
    list(APPEND req_components sump sump_io logic_capture perfetto_trace)
    list(APPEND srcs
        "virtual_perfetto_logic.c"
    )
endif()

if(CONFIG_USBIP_VIRTUAL_HARNESS)
    list(APPEND req_components harness_io_expander harness_gpio harness_bus)
    list(APPEND srcs
        "virtual_harness.c"
    )
endif()

if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND req_components esp_eth)
elseif(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND req_components esp_wifi esp_driver_gpio)
endif()

# Always compiled; neopixel_init() becomes a no-op when gpio < 0.
list(APPEND req_components esp_driver_rmt)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${req_components}
)

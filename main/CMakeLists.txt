idf_component_register(SRC_DIRS          "."
                       INCLUDE_DIRS      "."
                       REQUIRES          esp_matter esp_rcp_update esp_eth esp_netif esp_driver_gpio esp_driver_spi w5500 espressif__mdns esp_ot_br_server probe_http
                       LDFRAGMENTS "linker.lf")

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
target_link_options(${COMPONENT_LIB} INTERFACE
                    "SHELL:-u mdns_query_async_new"
                    "SHELL:-u mdns_delegate_hostname_add")

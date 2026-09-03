idf_component_register(
    SRCS "probe_http.c"
    INCLUDE_DIRS "include"
    REQUIRES probe_core espressif__cjson esp_http_server esp_timer esp_hw_support
)

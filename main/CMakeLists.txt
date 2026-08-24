idf_component_register(SRC_DIRS          "."
                       INCLUDE_DIRS      "."
                       LDFRAGMENTS "linker.lf")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

if(EXISTS "${CMAKE_SOURCE_DIR}/rcp_fw")
    spiffs_create_partition_image(rcp_fw "${CMAKE_SOURCE_DIR}/rcp_fw" FLASH_IN_PROJECT)
endif()

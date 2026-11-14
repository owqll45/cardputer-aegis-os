# 汉字点阵(cn12.bin)以二进制嵌入 flash，源文件里不放 9 万字节的数组
idf_component_register(SRCS "main.c" "lora.c" "ble_mesh.c" "display.c" "monitor.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "cn12.bin"
                       PRIV_REQUIRES mbedtls esp_driver_spi esp_driver_gpio esp_wifi esp_netif
                                     esp_event nvs_flash esp_adc esp_timer spi_flash bt)

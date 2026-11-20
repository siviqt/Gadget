idf_component_register(SRCS "services.c" "audio.c" "motion.c" "metrics.c" INCLUDE_DIRS "include"
                       REQUIRES gadget_sdk gadget_input freertos
                       PRIV_REQUIRES badgeteam__badge-bsp nicolaielectronics__tanmatsu-wifi
                                     nicolaielectronics__wifi-manager esp_driver_i2s esp_http_client
                                     esp_timer esp_wifi esp_netif mbedtls nvs_flash esp_hw_support heap lwip)

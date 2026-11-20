idf_component_register(SRCS "storage.c" "settings.c" "layout.c" INCLUDE_DIRS "include"
                       REQUIRES gadget_sdk gadget_dice espressif__cjson
                       PRIV_REQUIRES freertos esp_timer heap)

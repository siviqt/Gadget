idf_component_register(SRCS "input.c" INCLUDE_DIRS "include"
                       REQUIRES freertos gadget_sdk PRIV_REQUIRES badgeteam__badge-bsp gadget_dice esp_timer)

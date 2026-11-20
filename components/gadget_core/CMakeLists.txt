idf_component_register(SRCS "core.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES badgeteam__badge-bsp bootloader_support gadget_dice gadget_graphics gadget_input
                                     gadget_modules gadget_services gadget_config esp_timer esp_hw_support nvs_flash)

set(fonts "${CMAKE_CURRENT_LIST_DIR}/../../modules/ui_fonts")
idf_component_register(SRCS "display.c" "workspaces.c" "reel.c" "guide.c" "fonts.c"
                            "${fonts}/ibm_plex_sans_regular_14.c"
                            "${fonts}/ibm_plex_sans_regular_18.c"
                            "${fonts}/ibm_plex_sans_medium_20.c"
                            "${fonts}/ibm_plex_sans_semibold_24.c"
                            "${fonts}/ibm_plex_mono_semibold_56.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gadget_dice gadget_sdk
                       PRIV_REQUIRES badgeteam__badge-bsp robotman2412__pax-gfx
                                     esp_driver_ppa esp_lcd esp_timer freertos heap)

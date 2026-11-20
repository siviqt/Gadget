idf_component_register(SRCS "modules.c" "manifest.c" "native.c" "kbelf_port.c" INCLUDE_DIRS "include"
                       REQUIRES gadget_sdk gadget_config
                       PRIV_REQUIRES gadget_services badgeteam__badge-elf freertos hal)

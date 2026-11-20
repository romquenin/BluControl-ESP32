idf_component_register(SRCS 
                    "blu-analogs.c"
                    "blu-energy.c"
                    "blu-hardware.c"
                    "blu-rumble.c"
                    "dc_maple.c"
                    "gc_joybus.c"
                    "sticks/blu-analog-pad.c"
                    "sticks/blu-buttons-pad.c"
                    "sticks/blun64-pad.c"
                    "triggers/blu-analog-triggers.c"
                    INCLUDE_DIRS
                    "include"
                    "include/sticks"
                    "include/triggers"
                    REQUIRES app_update driver
                    esp_adc esp_partition esp_system
                    esp_timer spi_flash)

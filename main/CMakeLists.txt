idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES lcd app_update spi_flash wifi_sta)
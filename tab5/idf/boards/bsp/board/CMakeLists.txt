idf_component_register(SRCS "board_bsp.c" "usb_midi_host.c" INCLUDE_DIRS "." REQUIRES jiant esp_lcd esp_timer freertos esp_driver_i2s esp_driver_i2c nvs_flash)
# Pantalla girada 180 grados (USB-C por el otro lado). Para volver a la orientacion original, pon un # delante de la linea siguiente:
target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_ROTATE_B)

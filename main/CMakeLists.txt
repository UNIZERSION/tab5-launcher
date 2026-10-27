file(GLOB FONT_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/fonts/*.c")

idf_component_register(
    SRCS "main.c" "l5_board.c" "l5_display.c" "l5_config.c" "l5_slots.c" "l5_ptable.c" "l5_wifi.c" "l5_kbd.c" "l5_net.c"
         "l5_fw.c" "l5_inst.c" "l5_files.c"
         "ui/l5_theme.c" "ui/l5_ui.c" "ui/l5_rail.c" "ui/l5_home.c" "ui/l5_sys.c" "ui/l5_boot.c"
         "ui/l5_dialogs.c" "ui/l5_cover.c" "ui/l5_views.c" "ui/l5_widgets.c" "ui/l5_sd.c" "ui/l5_pman.c"
         "ui/l5_cfg.c" "ui/l5_keyboard.c" "ui/l5_wifi_ui.c" "ui/l5_inst_ui.c" "ui/l5_ota.c"
         "ui/l5_sd_ops.c" "ui/l5_charge.c" "ui/l5_keys.c" "ui/l5_dither.c" "ui/l5_appicon.c"
         ${FONT_SRCS}
    INCLUDE_DIRS "." "ui"
    PRIV_INCLUDE_DIRS "../bootloader_components/main" # l5_guard.h: the boot-loop guard's registers
    REQUIRES nvs_flash app_update bootloader_support spi_flash esp_partition esp_app_format json fatfs
             esp_driver_i2c sdmmc esp_timer mbedtls esp_psram esp_lcd esp_driver_ppa
             espressif__esp_lcd_touch esp_wifi esp_netif esp_event espressif__esp_hosted esp_http_client esp-tls
)

# The launcher's own code waits on touch, network and flash: built for size (the 2 MB factory
# partition holds it with OTA). The panel driver (DSI frames, cache-safe ISR) keeps -O2.
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
set_source_files_properties(l5_display.c PROPERTIES COMPILE_OPTIONS "-O2")

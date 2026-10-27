cmake_minimum_required(VERSION 3.16)

# Native LVGL port of the Tab5 Launcher HTML prototype (../tab5-launcher).
# The launcher lives in the `factory` partition; installed apps live in ota_N slots.
# bootloader_components/main overrides the second-stage bootloader so a power-on or
# RESET always lands in the launcher, while a software restart boots the app that
# the launcher selected in otadata (see README.md).
set(IDF_TARGET "esp32p4" CACHE STRING "M5Stack Tab5 target")
set(PROJECT_VER "0.1.0")
include(${CMAKE_CURRENT_LIST_DIR}/cmake/exfat.cmake)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_launcher)

# BSP 1.3.2 warns "Long filenames on SD card are disabled" by testing CONFIG_FATFS_LONG_FILENAMES,
# which is the name of a Kconfig choice and never becomes a macro. Long names are on here.
if(CONFIG_FATFS_LFN_HEAP OR CONFIG_FATFS_LFN_STACK)
    idf_component_get_property(_l5_bsp espressif__m5stack_tab5 COMPONENT_LIB)
    target_compile_definitions(${_l5_bsp} PRIVATE CONFIG_FATFS_LONG_FILENAMES=1)
endif()

# The launcher is a factory app in a fixed 2 MB partition: the Wi-Fi stack (lwIP, ESP-Hosted host) is
# built for size. The C6 does the radio work; the launcher only moves catalogues and firmware files.
foreach(_l5_net lwip espressif__esp_hosted esp_http_client esp-tls tcp_transport http_parser)
    idf_component_get_property(_l5_lib ${_l5_net} COMPONENT_LIB)
    target_compile_options(${_l5_lib} PRIVATE -Os)
endforeach()
# OTA's TLS client (mbedTLS libraries of the mbedtls component) too: 80 KB less than -O2 + full bundle
foreach(_l5_tls mbedtls mbedcrypto mbedx509)
    if(TARGET ${_l5_tls})
        target_compile_options(${_l5_tls} PRIVATE -Os)
    endif()
endforeach()

# ESP-Hosted starts itself from a constructor before app_main (port_esp_hosted_host_init.c). With 2.12.6
# the launcher's main task could not be created after it. l5_wifi.c calls esp_hosted_init() when Wi-Fi is
# first needed instead, so nothing of ESP-Hosted (SDIO slot, tasks, buffers) exists at boot.
idf_component_get_property(_l5_hosted espressif__esp_hosted COMPONENT_LIB)
get_target_property(_l5_hosted_srcs ${_l5_hosted} SOURCES)
list(FILTER _l5_hosted_srcs EXCLUDE REGEX "port_esp_hosted_host_init\\.c$")
set_target_properties(${_l5_hosted} PROPERTIES SOURCES "${_l5_hosted_srcs}")

# LVGL 9.3 decompresses glyphs of compressed fonts (from 24 px) with one global RLE state; the two software
# draw units (CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2) decompressed at the same time and garbled each other's glyphs
# until the next redraw. tools/patch_lvgl_rle.py writes a copy of src/font/lv_font_fmt_txt.c with the state
# on the stack into the build folder, and the lvgl component builds that copy instead of its own file
# (managed_components and ESP-IDF are not changed).
idf_build_get_property(_l5_python PYTHON)
idf_component_get_property(_l5_lvgl lvgl__lvgl COMPONENT_LIB)
idf_component_get_property(_l5_lvgl_dir lvgl__lvgl COMPONENT_DIR)
set(_l5_rle_src "${_l5_lvgl_dir}/src/font/lv_font_fmt_txt.c")
set(_l5_rle_out "${CMAKE_BINARY_DIR}/l5_components/lvgl/lv_font_fmt_txt.c")
execute_process(COMMAND ${_l5_python} "${CMAKE_CURRENT_LIST_DIR}/tools/patch_lvgl_rle.py" "${_l5_rle_src}" "${_l5_rle_out}"
                RESULT_VARIABLE _l5_rle_result)
if(NOT _l5_rle_result EQUAL 0)
    message(FATAL_ERROR "tools/patch_lvgl_rle.py could not patch ${_l5_rle_src}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_l5_rle_src}" "${CMAKE_CURRENT_LIST_DIR}/tools/patch_lvgl_rle.py")
get_target_property(_l5_lvgl_srcs ${_l5_lvgl} SOURCES)
list(FILTER _l5_lvgl_srcs EXCLUDE REGEX "/src/font/lv_font_fmt_txt\\.c$")
list(APPEND _l5_lvgl_srcs "${_l5_rle_out}")
set_target_properties(${_l5_lvgl} PROPERTIES SOURCES "${_l5_lvgl_srcs}")
message(STATUS "Tab5 Launcher: LVGL font RLE state per call (${_l5_rle_out})")

# App icons (main/ui/l5_appicon.c) only decode PNG files: LVGL's lodepng is built without its encoder, text chunks
# and error texts, and for size. l5_appicon.c calls only lodepng_decode32(), whose declaration does not depend on
# these switches (it reads the IHDR size itself instead of using LodePNGState, whose layout does). lv_lodepng.c
# needs lodepng_load_file(): the file functions stay.
target_compile_definitions(${_l5_lvgl} PRIVATE LODEPNG_NO_COMPILE_ENCODER LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
                           LODEPNG_NO_COMPILE_ERROR_TEXT)
set_source_files_properties("${_l5_lvgl_dir}/src/libs/lodepng/lodepng.c" TARGET_DIRECTORY ${_l5_lvgl}
                            PROPERTIES COMPILE_OPTIONS "-Os")

# exFAT for the SD card (most microSD cards above 32 GB ship formatted as exFAT).
# ESP-IDF 5.5.2 ships FatFs with FF_FS_EXFAT hard-coded to 0 and no Kconfig option, so a
# build-local copy of the fatfs component (implementation and public ff.h together) overrides
# the built-in one. The global ESP-IDF checkout is never patched.
# Same approach as ../aurora-player/cmake/exfat.cmake, which runs on this Tab5.
set(_l5_fatfs_source "$ENV{IDF_PATH}/components/fatfs")
set(_l5_fatfs_dir "${CMAKE_BINARY_DIR}/l5_components/fatfs")
if(NOT EXISTS "${_l5_fatfs_source}/src/ffconf.h")
    message(FATAL_ERROR "ESP-IDF missing. Activate ESP-IDF 5.5.2 before configuring.")
endif()
file(READ "${_l5_fatfs_source}/src/ffconf.h" _l5_ffconf)
if(NOT _l5_ffconf MATCHES "#define[ \t]+FF_FS_EXFAT[ \t]+0")
    message(FATAL_ERROR "Unexpected FatFs configuration in ESP-IDF. Use a pristine ESP-IDF 5.5.2.")
endif()
file(COPY "${_l5_fatfs_source}/" DESTINATION "${_l5_fatfs_dir}")
string(REGEX REPLACE "#define[ \t]+FF_FS_EXFAT[ \t]+0" "#define FF_FS_EXFAT 1" _l5_ffconf "${_l5_ffconf}")
file(WRITE "${_l5_fatfs_dir}/src/ffconf.h" "${_l5_ffconf}")
list(APPEND EXTRA_COMPONENT_DIRS "${_l5_fatfs_dir}")
message(STATUS "Tab5 Launcher: exFAT enabled in ${_l5_fatfs_dir}")

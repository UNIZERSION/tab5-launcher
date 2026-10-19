# Third-party parts

Tab5 Launcher's own code is under the MIT license (`LICENSE`). These parts come from other authors:

| Part | Where | Author | License |
|---|---|---|---|
| Chakra Petch, Orbitron and Share Tech Mono fonts | `tools/fonts/*.ttf` and the bitmaps in `main/fonts/*.c` (generated with lv_font_conv) | The Chakra Petch Project Authors; The Orbitron Project Authors; Carrois Type Design | SIL Open Font License 1.1, text in `tools/fonts/OFL.txt` |
| Font Awesome Free 5 icons | `main/fonts/l5_icon_*.c` (from the FontAwesome5 font bundled with LVGL) | Fonticons, Inc., https://fontawesome.com | font: SIL OFL 1.1, icons: CC BY 4.0 |
| Second-stage bootloader | `bootloader_components/main/bootloader_start.c` (modified copy of the ESP-IDF 5.5.2 file) | Espressif Systems | Apache-2.0 |
| Font decompression fix | `tools/patch_lvgl_rle.py` patches a copy of `lv_font_fmt_txt.c` from LVGL 9.3 during the build | LVGL LLC | MIT |
| Root certificates for HTTPS | `certs/ota_roots.pem` | certificate authorities | public certificates |

Downloaded during the build (not in the repository): ESP-IDF 5.5.2, the `espressif/m5stack_tab5` BSP, `esp_lvgl_port`, `esp_hosted` and `esp_wifi_remote` (Apache-2.0), and LVGL 9.3 (MIT).

#pragma once

/* Files from the PC to the SD card over the USB cable (the console port), without taking the card out:
   tools/sd_put.py on the PC. Commands, one line each, answered with lines that start with "L5PUT" / "L5RM":
     l5put <path> <size> <fnv1a-32 hex>   then the bytes; "L5PUT ACK <n>" after every 8 KB and at the end,
                                          "L5PUT OK <path> <size>" once the file is on the card
     l5rm <path>                          deletes a file
   <path> is on the card ("/wallpaper.jpg", "/apps/orbit_drum/icon.png"); missing folders are made. */
void l5_link_start(void);

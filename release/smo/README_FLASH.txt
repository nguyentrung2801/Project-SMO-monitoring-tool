SMO Monitor firmware release
======================================

Complete image for fresh installation (clears NVS)
--------------------------
File: SMO_Monitor_merged.bin
Flash address: 0x0

This file contains the bootloader, partition table, boot_app0 and application.
LittleFS is NOT included; blank boards also need pio run -t uploadfs.
Its padding covers the NVS region and overwrites saved settings and test state.
Do NOT use the merged image to update a device whose state must be preserved.
Use Espressif Flash Download Tool for a fresh installation only.

Application-only image
----------------------
File: SMO_Monitor.bin
Flash address: 0x10000

Use the application-only image only when the board already has the matching
bootloader and partition table.
This preserves NVS (Wi-Fi, test information, configuration, test state and ACKs).
Update an existing matching installation using:

    flash_smo.bat COM3

Target
------
Chip: ESP32-C3
Flash size: 4 MB
Flash mode: DIO
PlatformIO environment: esp32-c3-mini

Important
---------
Application-only updates and standard PlatformIO uploads with the same partition
layout preserve NVS and LittleFS. Do not erase flash or flash a merged image when
you want to keep data. A full erase removes settings, test state, ACKs and history.

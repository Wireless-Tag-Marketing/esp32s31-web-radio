# Codex project handoff

This file applies to the entire repository. Read it before changing, building, flashing, or diagnosing the project.

## Project goal and current state

This is a standalone ESP-IDF internet radio for the ESP32-S31-Korvo-1 V1.1 board. It uses the board's 800x480 RGB display, GT1151 capacitive touch controller, ES8389 codec, two NS4150 speaker amplifiers, Wi-Fi provisioning, and UART0 logs.

The current source version is `2.2.2-list-contrast` (2026-08-05). It builds successfully. The archived single-file firmware is `firmware/esp32s31-korvo1-webradio-v2.2.2-list-contrast.bin`, whose SHA-256 is:

```text
c69575347d384a669317ff645d8188672424cc32dddc85558360131d6648b273
```

The source was last built successfully, but flashing this exact `2.2.2` build was not completed because `/dev/ttyUSB0` disappeared. The connected board was therefore last confirmed running `2.2.1-ui-left`. On the next hardware session, flash `2.2.2` and visually verify the station-picker contrast.

## Toolchain and paths

- ESP-IDF: `/home/biubiu/.espressif/master/esp-idf`
- Target: preview target `esp32s31`
- Project root: `/home/biubiu/work/tmp/ESP32S31_KORVO1_RADIO`
- ESP-IDF examples: `/home/biubiu/.espressif/master/esp-idf/examples`
- Wi-Fi provisioning reference implementation: `/home/biubiu/work/WT_BSP/examples/wt_factory/wt9932p4c61-tiny/main`
- Board schematic archived at `docs/hardware/esp32-s31-korvo-1-schematics.pdf`

Do not assume that `/dev/ttyUSB0` exists. Check the port before flashing because the USB-UART device may re-enumerate.

## Build, package, flash, and monitor

The normal build and single-image packaging command is:

```bash
./tools/build_merged.sh
```

The script uses the ESP-IDF path above unless `ESP_IDF_ROOT` is set. It runs the preview build and creates:

```text
build/esp32s31-korvo1-webradio-merged.bin
```

Flash a development build while preserving NVS:

```bash
source /home/biubiu/.espressif/master/esp-idf/export.sh
idf.py -p /dev/ttyUSB0 flash -b 3000000
```

Monitor UART0 at 115200 baud:

```bash
source /home/biubiu/.espressif/master/esp-idf/export.sh
idf.py -p /dev/ttyUSB0 monitor
```

For Espressif Flash Download Tool, use the merged file only:

```text
Chip: ESP32-S31
Address: 0x0
SPI mode: DIO
SPI speed: 80 MHz
Flash size: 16 MB
```

A merged-file flash from offset `0x0` covers blank gaps and can erase NVS, so Wi-Fi provisioning may be required afterward. `idf.py flash` writes the individual bootloader, partition table, and application images and normally preserves NVS.

## Hardware facts

- Board: ESP32-S31-Korvo-1 V1.1
- Flash: 16 MB, configured for 80 MHz
- Octal PSRAM: 16 MB at 200 MHz
- Display: 800x480 RGB565 parallel RGB panel
- Touch: GT1151/GT1158, I2C SDA GPIO0, SCL GPIO1, address `0x14`
- Audio codec/amplifier: ES8389 plus dual NS4150
- I2S: BCLK GPIO3, LRCK GPIO4, DOUT GPIO5
- Speaker amplifier enable: GPIO7
- UART0 console: GPIO59 TX, GPIO58 RX, 115200 8-N-1

The ES8389 has no MCLK connection on this board. All decoded PCM is converted to 16 kHz, stereo, signed 16-bit before I2S output. The speaker connectors are differential NS4150 power outputs for passive speakers; neither `SPK+` nor `SPK-` may be grounded. Do not treat them as a PC-style 3.5 mm line/headphone output.

## Software architecture

- `main/main.c`: application event loop, NVS settings, Wi-Fi state, UI/audio event routing, UART commands, station switching, and retry policy.
- `main/audio_player.c`: HTTP/HTTPS/ICY source handling, playlist and HLS handling, lightweight MP3/AAC/TS decoding, sample conversion, PSRAM stream buffering, I2S writes, speaker test, and audio diagnostics.
- `main/radio_ui.c`: LVGL 9.4 touch UI, station modal, Wireless-Tag branding, transport/volume controls, Wi-Fi setup, and PCM-driven spectrum animation.
- `main/stations.c`: ordered station table. The first 8 entries are China-mainland CDN direct MP3 streams; the full table currently has 51 entries.
- `main/wifi_manager_bridge.cpp`: C bridge to `78/esp-wifi-connect`, matching the requested provisioning flow.
- `main/app_config.h`: defaults and station-list schema version.
- `sdkconfig.defaults`: reproducible project configuration. Generated `sdkconfig` is intentionally ignored.
- `dependencies.lock`: exact managed-component resolution and must be committed.

## Audio performance decisions

- Do not reintroduce ESP-GMF or `simple_player`; earlier GMF pipelines caused severe UI/audio latency and null-pipeline errors.
- Network reading runs on CPU0, while decoding/I2S runs on CPU1.
- The 256 KiB encoded stream buffer is explicitly allocated in PSRAM with `xStreamBufferCreateWithCaps(..., MALLOC_CAP_SPIRAM)` and must be deleted with `vStreamBufferDeleteWithCaps`.
- Startup buffering targets about 2 seconds for quick switching. Low-watermark recovery targets about 5 seconds. The remaining buffer fills in the background.
- Wi-Fi power saving is disabled during continuous streaming.
- A remote stream whose sustained throughput is below its encoded bitrate will eventually stall regardless of buffer size. Prefer the 64 kbps mainland direct MP3 stations when testing in China.
- The application emits startup buffering time and measured throughput in UART logs; use these values before changing buffer thresholds.

## UI requirements

- LVGL must stay on 9.4.x unless a deliberate migration is requested.
- Keep the main title `WebRadio By Wireless-Tag` and the Wireless-Tag visual emphasis.
- The app title and the `Tap to browse | Swipe to tune` hint are left aligned.
- The equalizer bars use decoded PCM peak data and should animate only while audio is active.
- The station picker uses explicit high-contrast child-label styles because LVGL button theme styles otherwise override the list container's text color.
- Current station picker palette in `2.2.2`: `#FAFBFF` text, `#12D6C5` icons, `#222C43` row background, `#35415F` border, and purple pressed state.

## Wi-Fi and station behavior

- Provisioning UI language is Chinese and the AP prefix is `WEBRADIO`.
- Station list schema is currently version 2. When an old schema is detected, the application selects station 0 while preserving Wi-Fi credentials and volume.
- Station 0 is `CNR-1 Voice of China`.
- UART commands: `r` retry, `n/p` next/previous station, `+/-` volume, `w` provisioning, `t` speaker test, `?` diagnostics.

Never commit Wi-Fi credentials, captured NVS partitions, private certificates, or user-specific serial logs.

## Expected boot/runtime evidence

A healthy boot should include all of the following:

```text
App version:      2.2.2-list-contrast
Display initialized with resolution 800x480
Touch UI ready: 800x480 RGB565, GT1151, LVGL 9.4.0, author Wireless-Tag
Korvo-1 audio ready: ES8389 + NS4150
Lightweight MP3/AAC/TS decoder registered (GMF disabled)
Encoded buffer allocated in PSRAM: 256 KiB
Startup buffered: ...
Lightweight PCM reached ES8389: ...
```

For the mainland stations, the previous hardware test showed zero `buffer_underruns` and startup buffering in hundreds of milliseconds after `HTTP 200`.

## Verification checklist for changes

1. Keep unrelated source and configuration changes intact.
2. Update `PROJECT_VER` in the root `CMakeLists.txt` for a user-visible firmware change.
3. Run `./tools/build_merged.sh`.
4. Verify `esptool image-info` reports ESP32-S31, DIO, 80 MHz, 16 MB, valid checksum/hash, and the intended app version.
5. If hardware is available, flash with the specified 3,000,000 baud command and monitor UART0.
6. For audio work, confirm PCM reaches ES8389 and `buffer_underruns` remains zero on a mainland station.
7. For UI work, visually inspect the 800x480 display and exercise touch, modal scrolling, station selection, play/pause, volume, and Wi-Fi setup.
8. Regenerate the single merged firmware and update the tracked release artifact plus its SHA-256 in `firmware/README.md` when preparing a new archive/release.

## Git hygiene

Do not commit `build/`, `managed_components/`, generated `sdkconfig`, editor state, or logs. They are ignored. Commit source, `sdkconfig.defaults`, `dependencies.lock`, documentation, the schematic, and explicitly versioned release firmware under `firmware/`.

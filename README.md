# SMO Monitor - ESP32-C3 PCB

Firmware for monitoring SMO door controllers over UART and MQTT. Built with Arduino and PlatformIO for ESP32-C3, with a local web interface for setup and test control. Current firmware version: `1.0.0`.

## Hardware

| Function | Configuration |
| --- | --- |
| PlatformIO environment | `esp32-c3-mini` |
| Board | `esp32-c3-devkitm-1`, 4 MB flash |
| SMO UART RX | GPIO5; connect to SMO TX |
| SMO UART TX | GPIO6; connect to SMO RX |
| UART format | 115200 baud, 8N1 |
| Relay outputs | GPIO3 and GPIO0, active LOW |
| WB350 transistor output | GPIO7, active HIGH |
| Button pulse | 200 ms |

All three button outputs pulse together. Each pulse counts as one button press. UART TX open-drain mode is optional and disabled by default.

GPIO7 drives an external NPN transistor: GPIO7 through 1 kOhm to base, collector to WB350, emitter to common ground, and 10 kOhm from base to emitter. Never connect the WB350 7-8 V line directly to an ESP GPIO. This output simulates the open/close button, not the full WB350 command set.

Use suitable relay drivers and compatible UART voltage levels. See [WB350 wiring](docs/wb350-output.md).

## Setup and web interface

1. Connect to `SMO-Monitor-xxxxxx`, password `12345678`.
2. Open `http://4.4.4.4`.
3. Save test information: device name, tester, location, door type and motor type. The four fields other than device name are required by the firmware.
4. Select and connect to a Wi-Fi network. Manual SSID entry is supported.
5. Choose Cloud or Local, then select **Save and restart**.
6. Configure the cycle interval and optional button-press limit, then start the test.

The web interface shows Wi-Fi details, MQTT readiness, test state, button count and elapsed time. It supports Start, Pause, Resume, Stop and Manual Trigger. Settings are stored in NVS.

Changing server restarts the ESP after about two seconds and temporarily disconnects its Wi-Fi AP. The form keeps the selected server and locks while restarting. Reconnect and reload the page before changing it again. Stop the test before switching.

The gateway ID and MQTT client ID are derived from the MAC address. The device name is a separate display label. The cloud dashboard is `https://sklsmomonitor.web.app/`.

## MQTT servers

| Server | Address | Transport |
| --- | --- | --- |
| Cloud | `b67168d5.ala.asia-southeast1.emqxsl.com:8883` | MQTT over TLS with CA verification |
| Local | `192.168.223.230:1883` | Plain MQTT |

Broker addresses, credentials and the cloud CA certificate are defined in `src/config.h`. The local address cannot be edited through the web interface. The selected server survives reboot; older saved local addresses are ignored.

MQTT starts after Wi-Fi connects, required test information is saved and a valid system time is available. NTP uses `pool.ntp.org` and `time.google.com`; Local mode also requires this time check. The current implementation does not provide a fully offline clock setup.

MQTT uses a 10-second network timeout and a 30-second keepalive. The web interface reports ONLINE after all three command-topic subscriptions are acknowledged.

Topic prefix: `smo_aging/v1/{gateway_id}/`.

| Topic | QoS | Retained | Purpose |
| --- | --- | --- | --- |
| `up/availability` | 1 | Yes | Online heartbeat every 30 seconds; offline Last Will |
| `up/info` | 1 | No | Device name, test information and revision |
| `up/state` | 1 | Yes | Test state, session, count and settings |
| `up/config` | 1 | No | Settings saved from the web interface |
| `up/frame` | 1 | No | Raw frame, decoded fields and capture time |
| `up/uart_log` | 0 | No | Batched TX/RX debug logs |
| `up/ack` | 1 | No | Command acceptance and result |
| `down/control` | 1 | No | Test and button commands |
| `down/config` | 1 | No | Cycle interval and press limit |
| `down/smo_command` | 1 | No | ASCII or hexadecimal UART commands |

Last Will timestamps are fixed when the MQTT client is created. The server should use receipt time and heartbeat expiry to detect stale Online status. Broker acknowledgement does not confirm database storage or application processing.

## Test control

- `START_TEST`: resets the count and triggers the first pulse immediately. Requires a configured cycle. Uses the supplied session ID, or the command ID when omitted.
- `PAUSE_TEST` / `STOP_TEST`: stop automatic pulses and release an active pulse. Stop is reported as `idle`.
- `RESUME_TEST`: resumes the schedule after one cycle interval.
- `TRIGGER_MANUAL`: triggers one pulse; also works outside a running test.
- `RESET_COUNT`: resets the counter when the test is neither running nor paused; available through MQTT.

Cycle interval: 1-86400 seconds. Press limit: 0-1000000; zero means unlimited. Manual presses count toward the limit while running. Reaching the limit sets the state to `completed`.

State, settings, session and count are saved before a pulse is asserted. After reboot, a saved running test resumes after one interval. Missed or interrupted pulses are not replayed. Elapsed time includes pauses but excludes power-off time; recovery uses the last saved checkpoint.

Web configuration changes are marked pending until MQTT PUBACK. On reconnect, the gateway publishes its current state rather than replaying local control actions.

## UART frames and fault filtering

Status frames use this 11-byte format:

```text
AA AA 01 02 03 02 STATUS FAULT CS 55 55
```

`CS` is the XOR of bytes 2-7. Supported fault bytes are `00` through `07`. Invalid length, checksum, status or unsupported fault values are rejected and counted.

Decoded fields include door state, direction, light, remote enabled, auto-closer countdown, IR blocked and fault code. Reported faults are E1, E2, E3, E5, E6 and E7; E4 is excluded from fault reporting.

Complete frames are deduplicated within 100 ms of the last accepted identical frame. Suppressed copies do not extend that window. Reportable errors wait 100 ms for confirmation. A valid No Error frame cancels an error only if received less than 40 ms after it. Distinct pending errors are handled independently.

SB frames matching `AA AA 01 01 00 ... 55 55` are filtered out. Other supported SB notifications are forwarded as raw frames with unknown decoded fields. Binary traffic is separated from ASCII debug logs.

UART decoding remains active outside tests. Only frames received while the test is running are eligible for forwarding. Starting or resuming clears pending parser/filter state. Frame timestamps are captured at receipt with second resolution; log and ACK timestamps include milliseconds in UTC.

## Storage and delivery

Live frames are queued directly to MQTT when ready and within the memory budget. LittleFS stores fallback frames when direct queuing is unavailable.

- The flash outbox holds up to 200 frames. New records are dropped and counted when full or unavailable.
- File names are indexed in RAM. The directory is scanned at startup, not before every publish.
- Stored frames are sent while the test is running and MQTT is ready, then removed after PUBACK.
- Paused or stopped tests retain pending files. Live traffic may arrive before older stored frames.
- Switching server also changes the destination of pending frames.
- The persistent fault history keeps up to 50 reportable faults.

LittleFS is mounted without automatic formatting. Mount failure leaves saved NVS configuration intact. Direct MQTT delivery can still work, but flash fallback cannot.

For maintenance, enable `SMO_CLEAR_OUTBOX_ON_BOOT` in `src/config.h`, flash and boot once, then disable it and flash again. It deletes pending outbox JSON files; it does not format LittleFS or clear test settings.

## UART debug commands

Server commands specify `schema`, `cmd_id`, `action`, `encoding`, `data`, `issued_at` and `expires_at`. The gateway rejects retained, expired, future-dated or malformed commands. Limits are 2048 bytes of command JSON and 256 UART bytes, including added delimiters.

ASCII commands are framed with leading and trailing carriage returns (`0D`). Hex commands send the supplied bytes. Action labels do not generate SMO command bytes automatically.

Successful UART commands open continuous ASCII capture. Sending `DISABLE` with optional CR/LF closes it; reboot starts with capture closed. Binary frames and filtered fragments do not become text logs.

Logs use chunks up to 192 bytes, flushed at newline, buffer capacity or 10 ms idle. Batches are sent after approximately 50 ms or four entries. A separate task sends QoS 0 packets through a three-packet RAM queue.

The current log queue waits when full and retries failed packets. If MQTT stays offline while debug output continues, this can block the main loop and web interface. RAM is bounded, but end-to-end lossless logging is not guaranteed. Logs are not persisted to flash.

ACKs distinguish command receipt from `sent_to_smo`; UART transmission does not prove SMO execution. The last 16 command IDs are cached in NVS to prevent immediate replay. EEPROM decoding and transaction management are not implemented.

## Local API and diagnostics

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/api/status` | GET | Wi-Fi, MQTT, storage, timing, counters and latest frame |
| `/api/info` | GET / POST | Read or save test information |
| `/api/wifi` | POST | Save Wi-Fi credentials and connect |
| `/api/wifi/scan` | GET | Scan nearby networks |
| `/api/mqtt/server` | POST | Select `cloud` or `local` and restart |
| `/api/gateway/config` | POST | Save cycle and limit |
| `/api/gateway/control` | POST | Apply local test controls |
| `/api/errors` | GET | Read fault history |
| `/api/uart` | GET | Read UART diagnostics and recent RAM log preview |

Status includes reset reason, boot ID, uptime, heap, loop delays, flash timings, queue counts and MQTT transport/TLS errors. `mqtt_connected` means command subscriptions are ready; `wifi_connected` alone does not establish server connectivity.

## Build and flash

Run from the project directory with PlatformIO installed:

```powershell
pio run
pio run -t upload
```

Initialize LittleFS on a blank board when needed:

```powershell
pio run -t buildfs
pio run -t uploadfs
```

`uploadfs` replaces the filesystem and erases stored frames/history. Do not use it for ordinary firmware updates.

Build release files on Windows:

```powershell
.\scripts\build_release.bat
```

The script writes files to `release/smo/`:

- `SMO_Monitor.bin`: application image, flash at `0x10000`.
- `SMO_Monitor_merged.bin`: bootloader, partition table and application, flash at `0x0`.
- `flash_smo.bat` and `README_FLASH.txt`: application flashing helper and instructions.

The merged image does not include LittleFS and can overwrite NVS. Use application-only uploads with the same partition layout to preserve configuration and stored data. The regular build output is `.pio/build/esp32-c3-mini/firmware.bin`.

## Source layout

| File | Role |
| --- | --- |
| `src/config.h` | Pins, brokers, credentials, certificate and limits |
| `src/main.cpp` | Startup and main loop |
| `src/gateway.cpp` | UART, MQTT, storage, diagnostics and commands |
| `src/button_controller.*` | Button pulses, test scheduling and persistent state |
| `src/frame_decoder.h` | Status decoding |
| `src/frame_filter.h`, `src/frame_deduplicator.h` | Fault timing and duplicate filtering |
| `src/sb_frame.h` | SB frame recognition |
| `src/protocol.h`, `src/uart_log_capture.h` | Command bytes and capture policy |
| `src/time_manager.*` | NTP and UTC timestamps |
| `src/web.cpp` | Web interface and HTTP API |
| `scripts/` | Build and release helpers |

The backend, MQTT broker and dashboard are separate applications. Verify wiring, pulse timing, reconnect behavior and sustained UART traffic on the actual hardware before unattended tests.

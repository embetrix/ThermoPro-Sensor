# ThermoPro sensor reader

`sensor` is a Linux C program that finds ThermoPro sensors through BlueZ.
It listens for Bluetooth LE broadcasts and prints one JSON line per sensor.
The default report period is 10 seconds. You do not need to enter sensor
addresses, pair devices, or install Python to run it.

## Build and run with CMake

On Ubuntu/Debian, install the build dependencies and Bluetooth service:

```sh
sudo apt install build-essential cmake pkg-config libsystemd-dev libjson-c-dev bluez
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/sensor sensors.json
```

BlueZ must be running and the selected Bluetooth adapter must be on.
Press Ctrl+C or send SIGTERM to stop the program. It closes its scan session
on exit. Status and error messages go to stderr. JSON readings go to stdout.

## Source layout

Source files are in `src/`:

| File | Purpose |
| --- | --- |
| `main.c` | Read command-line options and handle stop signals |
| `app_config.c` / `app_config.h` | Read and check JSON settings |
| `bluez_scanner.c` / `bluez_scanner.h` | Scan with BlueZ, read D-Bus events, and time reports |
| `thermopro_decoder.c` / `thermopro_decoder.h` | Check and decode packets, with adjustable matching values |
| `sensor_registry.c` / `sensor_registry.h` | Track sensors and manage their memory |
| `json_output.c` / `json_output.h` | Print readings as JSON |
| `sensor.h` | Define the shared sensor data structure |

CMake builds config, decoder, registry, and output code into `thermopro_core`.
The program and unit tests share this library. The program also links the
BlueZ scanner. Config settings and sensor data are stored separately.

## Configuration

`sensors.json` uses automatic discovery only:

```json
{
  "discover_sensors": true,
  "polling_period_seconds": 10,
  "DuplicateData": true,
  "adapter": "hci0"
}
```

- `discover_sensors`: optional, defaults to `true`. Setting it to `false` is an
  error. A sensor is added after its first matching packet. Its full MAC address
  becomes its `sensor` name.
- `polling_period_seconds`: required whole number of seconds, at least 1.
  Scanning runs all the time. This setting controls how often readings are
  printed. Output starts at the next report after a sensor is found.
- `DuplicateData`: optional boolean, defaults to `true`. Set `false` to let
  BlueZ filter out repeated packets. Unchanged readings may then produce no
  update, so a report can show `null`.
  See the [BlueZ discovery filters](https://bluez.readthedocs.io/en/latest/adapter-api/).
- `adapter`: optional adapter name, defaults to `hci0`.

This is JSON: use lowercase `true`/`false` and no trailing commas.
Restart after changing the config. If you omit the path, the program reads
`sensors.json` from the current directory.

Check the config without using Bluetooth:

```sh
./build/sensor --check-config sensors.json
```

Example output:

```json
{"sensor":"EC:20:1E:58:25:10","reading":{"received_at":"2026-11-07T09:00:00.000000+00:00","temperature_c":23.5,"humidity_pct":48,"battery_low":false,"rssi_dbm":-67}}
{"sensor":"FA:5B:AB:FB:56:D6","reading":null}
```

Each report prints the latest reading, then clears it. If a known sensor sends
no matching packet before the next report, its `reading` is `null`.
There is no JSON output until a sensor is found. Timestamps use UTC and
temperatures have one decimal place. Missing RSSI also shows as `null`.

## Automatic ThermoPro discovery

New sensors appear while the program runs. Each new sensor is also logged to
stderr with its MAC address. Addresses stay in memory until exit and are not
saved to the config. Discovery starts fresh after a restart.

By default, a matching packet has seven manufacturer data bytes, including
the two-byte key. It starts with `0xC2` and has a humidity value from 0 to 100.
You can change these checks through CMake.

Discovery uses the packet format to find sensors. It cannot confirm the brand
or model. Some ThermoPro models may use other formats, and another device could
send a matching packet. Sensors must be in range and broadcasting.

## Override packet matching with CMake

Packet checks use named `#define` values in `src/thermopro_decoder.h`.
Each default has an `#ifndef` guard. Set the CMake options to change them
without editing the source:

```sh
cmake -S . -B build \
  -DTHERMOPRO_PAYLOAD_LENGTH=5 \
  -DTHERMOPRO_PACKET_MARKER=0xC2 \
  -DTHERMOPRO_MAX_HUMIDITY=100
cmake --build build
```

The example shows the defaults. Replace them as needed using decimal or hex
whole numbers.

| CMake option | Default | Meaning | Allowed range |
| --- | --- | --- | --- |
| `THERMOPRO_PAYLOAD_LENGTH` | `5` | Payload bytes without the two-byte manufacturer key | 3–65535 |
| `THERMOPRO_PACKET_MARKER` | `0xC2` | Required low byte of the manufacturer key | 0–255 |
| `THERMOPRO_MAX_HUMIDITY` | `100` | Highest allowed humidity percentage | 0–100 |

Rebuild and restart after changing these settings. CMake saves them in the
build directory. They only change packet matching. A different field layout
or temperature scale needs decoder changes too. Tests use your chosen values.

## Tests

Unit tests check the config, packet decoding, and JSON output:

```sh
ctest --test-dir build --output-on-failure
```

The optional integration test uses a private D-Bus and a fake BlueZ service.
It needs Python and `dbus-fast`, but no Bluetooth hardware:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install dbus-fast
cmake -S . -B build -DENABLE_BLUEZ_INTEGRATION_TESTS=ON -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build
ctest --test-dir build --output-on-failure
```

This test also needs `dbus-daemon` from the `dbus` package on Ubuntu/Debian.
It checks both `DuplicateData` settings, discovery, report timing, missing
readings, and shutdown. Test live readings with your Bluetooth adapter and sensors.

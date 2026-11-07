# ThermoPro sensor reader

`thermopro-sensor` is a Linux C program that finds ThermoPro sensors through BlueZ.
It listens for Bluetooth LE broadcasts, prints one JSON line per sensor, and
can save readings to SQLite and send them to ThingsBoard over MQTT.
The default report period is 10 seconds. You do not need to enter sensor
addresses, pair devices, or install Python to run it.

## Build and run with CMake

On Ubuntu/Debian, install the build dependencies and Bluetooth service:

```sh
sudo apt install build-essential cmake pkg-config libsystemd-dev libjson-c-dev libsqlite3-dev libmosquitto-dev ca-certificates bluez
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/thermopro-sensor config/thermopro-sensor.json
```

The build needs libmosquitto 2.0.12 or later.
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
| `sqlite_store.c` / `sqlite_store.h` | Save readings and delete expired history |
| `thingsboard_mqtt.c` / `thingsboard_mqtt.h` | Send readings through the ThingsBoard gateway API |
| `sensor.h` | Define the shared sensor data structure |

CMake builds config, decoder, registry, output, and database code into `thermopro_core`.
The program and unit tests share this library. The program also links the
BlueZ scanner. Config settings and sensor data are stored separately.

`config/` holds the example config. `deploy/` holds the systemd unit.

## Configuration

`config/thermopro-sensor.json` uses automatic discovery only:

```json
{
  "discover_sensors": true,
  "polling_period_seconds": 10,
  "DuplicateData": true,
  "adapter": "hci0",
  "database": {
    "path": "readings.db",
    "retention_days": 30,
    "max_size_mb": 10,
    "logging_mode": "per_report"
  },
  "mqtt": {
    "enabled": false,
    "host": "mqtt.thingsboard.cloud",
    "port": 8883,
    "tls": true,
    "access_token": "",
    "max_pending_sensors": 256
  }
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
- `database`: optional SQLite settings. Leave it out to turn off database
  logging. JSON output stays on.
- `mqtt`: optional ThingsBoard settings. Leave it out or set `enabled` to
  `false` to turn off MQTT.

This is JSON: use lowercase `true`/`false` and no trailing commas.
Restart after changing the config. If you omit the path, the program reads
`config/thermopro-sensor.json` from the current directory.

Check the config without opening Bluetooth, SQLite, or MQTT connections:

```sh
./build/thermopro-sensor --check-config config/thermopro-sensor.json
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

## SQLite history

The example config saves readings to `readings.db`. It deletes readings older
than 30 days and removes the oldest readings when the database needs space
under its 10 MB limit.

| Setting | Meaning | Default |
| --- | --- | --- |
| `path` | Required file path, from 1 to 4095 bytes | None |
| `logging_mode` | `per_report` or `every_packet` | `per_report` |
| `retention_days` | Days to keep, from 1 to 2147483647 | No age limit |
| `max_size_mb` | Size limit from 1 to 2147483647 MB, with 1 MB = 1,000,000 bytes | No size limit |

Omit either limit to turn it off. Zero, negative numbers, fractions, and
unknown settings are rejected. Omit the whole `database` object to turn off
SQLite logging. JSON output stays on.

A relative path starts from the directory where the program runs. The program
creates the database and tables. The parent directory must already exist and
allow writes. Use a local file path. SQLite URIs and `:memory:` are not supported.

- `per_report` saves the latest fresh reading from each sensor every
  `polling_period_seconds`. Missing readings create no rows. Readings waiting
  for the next report are lost if the program exits.
- `every_packet` saves each valid reading from BlueZ, including repeated values.
  Invalid packets and RSSI-only updates create no rows. Set `DuplicateData` to
  `true` to receive repeated values. This mode writes more often and uses more
  space. JSON output still follows the report interval.

Each report batch, or each D-Bus event in `every_packet` mode, is saved in one
transaction. A failed write undoes the whole batch.

Age cleanup runs at startup and hourly, even without new readings. It uses
UTC `received_at` and counts each day as 24 hours. Readings exactly on the
cutoff stay in the database.

When a write reaches the size limit, the program deletes old readings in
small batches and retries. It deletes by `received_at`, then by `id` for equal
times. This can remove readings younger than `retention_days`. If the new
batch still cannot fit after removing old history, the program reports an
error and stops.

A database already above the limit is shrunk at startup. This may delete old
readings, take time, and need temporary disk space. During normal logging,
SQLite reuses freed space, so deleting rows does not always shrink the file.

The size limit covers the `.db` file, including tables and indexes. It rounds
down to whole SQLite pages. WAL, shared-memory, and temporary files use extra
space. Use this program as the only writer because the limit applies to its
SQLite connection. See [SQLite's page limit](https://www.sqlite.org/pragma.html#pragma_max_page_count).

[WAL mode](https://www.sqlite.org/wal.html) lets other local programs read while
logging continues. Writes use `synchronous=FULL`. The program waits up to one
second for a database lock. Other database errors go to stderr and stop the
program. Systemd restarts it. Unsaved readings are lost.

The `readings` table has `id`, `sensor_address`, `received_at`,
`temperature_tenths`, `humidity_pct`, `battery_low`, and `rssi_dbm`.
Temperature is an integer in tenths of a degree Celsius. Battery status is
0 or 1. Missing RSSI is NULL. Indexes speed up sensor/time queries and cleanup.

Read the latest values with the optional `sqlite3` command-line tool:

```sh
sqlite3 -header -column readings.db '
SELECT sensor_address, received_at,
       temperature_tenths / 10.0 AS temperature_c,
       humidity_pct, battery_low, rssi_dbm
FROM readings ORDER BY received_at DESC LIMIT 20;'
```

## ThingsBoard MQTT

The host, port, TLS settings, and token come from the `mqtt` JSON object.
Changing them needs a restart, not a rebuild. The sample config leaves MQTT off.

Copy the sample to `config/thermopro-sensor.local.json`, set `enabled` to `true`, and
fill in the gateway's access token. This local file is ignored by Git.
Keep it readable only by your user with `chmod 600 config/thermopro-sensor.local.json`.

| Setting | Meaning | Default |
| --- | --- | --- |
| `enabled` | Turn MQTT on or off | Off without an `mqtt` object, otherwise on |
| `host` | MQTT hostname or IP address, without a URL scheme | Required when enabled |
| `port` | Broker port from 1 to 65535 | 8883 with TLS, otherwise 1883 |
| `tls` | Use TLS and check the server certificate and hostname | `true` |
| `access_token` | Gateway access token, sent as the MQTT username | Required when enabled |
| `ca_file` | Optional path to a trusted CA certificate file | Use OS certificates |
| `max_pending_sensors` | Maximum sensors waiting to send, from 1 to 10000 | 256 |

For `thingsboard.cloud`, use `mqtt.thingsboard.cloud` with TLS on port 8883.
For the EU service, use `mqtt.eu.thingsboard.cloud`. These are config values,
not fixed values in the C code. See ThingsBoard's
[Cloud MQTT settings](https://thingsboard.io/docs/paas/reference/mqtt-api/getting-connected/).

Run with your local settings:

```sh
./build/thermopro-sensor --check-config config/thermopro-sensor.local.json
./build/thermopro-sensor config/thermopro-sensor.local.json
```

The ThingsBoard device must have **Is gateway** enabled. Each sensor uses its
full MAC address as its device name. The reader sends `v1/gateway/connect`,
waits for its acknowledgement, then sends `v1/gateway/telemetry`. Both use
QoS 1, so retries can deliver a reading more than once. See the
[ThingsBoard gateway API](https://thingsboard.io/docs/reference/gateway-api/overview/).

MQTT sends the latest fresh reading at each `polling_period_seconds` interval,
regardless of the SQLite logging mode. It sends `temperature_c`, `humidity_pct`,
`battery_low`, and RSSI when available. Each reading keeps its receive time as
UTC Unix milliseconds. Missing readings create no MQTT message.

Network work runs in a separate thread. During an outage, SQLite and JSON
output continue. MQTT keeps the latest waiting reading per sensor, plus the
message being sent. New readings replace older waiting values. If all slots
are full, readings from other sensors are skipped with a message on stderr.
Network retries wait from 1 to 30 seconds. Fix certificate or login errors and
restart if the MQTT thread stops.

MQTT status goes to stderr, leaving stdout for JSON readings. Startup logs
whether MQTT is disabled or connecting to the configured host and port.
Connection failures include the error reason. While disconnected, each report
checks whether 30 seconds have passed since the last status message and logs
the number of sensors waiting. This also works with no sensor readings or while
a connection attempt is stalled. A stopped network thread is reported separately.
Successful connections and lost connections are logged too.
For the systemd service, follow these messages with
`journalctl -u thermopro-sensor -f`.

Waiting MQTT readings are lost on exit. The reader does not upload old rows
from SQLite. It does not mark a sensor offline after a missed report or
support the IoT Gateway's remote configuration and connector settings.

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

## Run with systemd

Build the program first. The unit needs systemd 247 or later.
Run these commands from the project root:

```sh
sudo install -m 0755 build/thermopro-sensor /usr/local/bin/thermopro-sensor
sudo install -d -m 0755 /etc/thermopro-sensor
sudo install -m 0600 config/thermopro-sensor.json /etc/thermopro-sensor/thermopro-sensor.json
sudo install -m 0644 deploy/thermopro-sensor.service /etc/systemd/system/thermopro-sensor.service
sudo systemctl daemon-reload
sudo systemctl enable --now thermopro-sensor.service
```

For MQTT, use `config/thermopro-sensor.local.json` in the config copy command above.
The unit uses `LoadCredential` so its service user can read the private config.

The config copy command replaces an existing config. Skip that command when
upgrading if you want to keep your installed settings.
If your installed config is still named `sensors.json`, rename it to
`thermopro-sensor.json` in `/etc/thermopro-sensor` before using the updated unit.

The unit creates `/var/lib/thermopro-sensor` with
`StateDirectory=thermopro-sensor` and runs from there. The example database
path becomes `/var/lib/thermopro-sensor/readings.db`. History stays after a
restart. If you change the path, give the service user access to its parent
directory.

The unit uses a temporary service user created by systemd. It talks to BlueZ
through the system D-Bus, so that user needs permission to call `org.bluez`.
The standard BlueZ D-Bus policy on Ubuntu/Debian allows this. The selected
adapter must be on before scanning can start.

The service starts after BlueZ and restarts after a failure. It stops when
BlueZ stops. Start it again after bringing BlueZ back. Readings and errors go
to the journal. See the [systemd service settings](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html).

```sh
sudo systemctl status thermopro-sensor.service
sudo journalctl -u thermopro-sensor.service -f -o cat
```

Edit `/etc/thermopro-sensor/thermopro-sensor.json` to change service settings, then check
the config and restart:

```sh
sudo /usr/local/bin/thermopro-sensor --check-config /etc/thermopro-sensor/thermopro-sensor.json
sudo systemctl restart thermopro-sensor.service
```

## Tests

Tests cover config checks, packet decoding, JSON output, and saved readings.
They also check MQTT config and payloads, failed writes, age and size limits,
and database shrinking:

```sh
ctest --test-dir build --output-on-failure
```

The optional integration test uses a private D-Bus and a fake BlueZ service.
It needs Python and `dbus-fast`, but no Bluetooth hardware:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install dbus-fast
cmake -S . -B build -DENABLE_BLUEZ_INTEGRATION_TESTS=ON \
  -DENABLE_MQTT_INTEGRATION_TESTS=ON -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build
ctest --test-dir build --output-on-failure
```

This test also needs `dbus-daemon` from the `dbus` package on Ubuntu/Debian.
It checks both `DuplicateData` settings, discovery, report timing, missing
readings, both database logging modes, MQTT output, and shutdown. The MQTT
test uses Python and `openssl` to check acknowledgements, reconnects, queue
limits, and TLS on local sockets. Tests do not contact ThingsBoard. Test live
readings with your Bluetooth adapter and sensors.

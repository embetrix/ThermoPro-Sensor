# ThermoPro sensor reader

Linux C program that discovers compatible ThermoPro Bluetooth LE sensors,
prints JSON readings, and optionally logs to SQLite and ThingsBoard MQTT.
No sensor addresses or pairing needed.

## Supported hardware

| Model | Readings | Product page |
| --- | --- | --- |
| ThermoPro TP357S | Temperature, humidity, battery status | [TP357 series](https://buythermopro.in/products/thermopro-tp357-indoor-bluetooth-hygrometer-thermometer) |

## Build and run

On Ubuntu/Debian:

```sh
sudo apt install build-essential cmake pkg-config libsystemd-dev libjson-c-dev libsqlite3-dev libmosquitto-dev ca-certificates bluez
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/thermopro-sensor config/thermopro-sensor.json
```

Requires libmosquitto 2.0.12 or later, BlueZ running, and Bluetooth powered on.
Press Ctrl+C to stop. JSON goes to stdout, status and errors to stderr.

## Configuration

Edit [config/thermopro-sensor.json](config/thermopro-sensor.json), then restart.
This is also the default path when no config argument is given.

| Setting | Purpose |
| --- | --- |
| `polling_period_seconds` | Report interval in whole seconds, at least 1 |
| `DuplicateData` | Keep `true` to receive repeated readings |
| `adapter` | Bluetooth adapter, normally `hci0` |
| `database.path` | SQLite file, relative to the working directory unless absolute |
| `database.logging_mode` | `per_report` for latest readings, or `every_packet` |
| `database.retention_days` | Delete readings older than this many days, omit for no age limit |
| `database.max_size_mb` | Delete oldest readings to stay within the DB size limit, omit for no size limit |

Scanning is continuous. A known sensor with no new packet in a report interval
produces `"reading": null`. Timestamps use UTC and temperatures use °C with one
decimal place. Missing readings are not saved to SQLite or sent over MQTT.

The sample enables SQLite with 30 days of history and a 10 MB limit.
The size limit covers the main DB file only, WAL and temporary files need extra
space. The database directory must exist and be writable. Remove the `database`
object to disable logging. Database write errors stop the program.

Check settings without connecting to Bluetooth or MQTT:

```sh
./build/thermopro-sensor --check-config config/thermopro-sensor.json
```

## ThingsBoard MQTT

**The sample config disables MQTT.** Create a private copy named
`config/thermopro-sensor.local.json` if you do not already have one.
This filename is ignored by Git. Set these fields inside `mqtt`:

- `enabled`: `true`
- `host`, `port`, `tls`: your broker settings
- `access_token`: your ThingsBoard gateway token

The ThingsBoard device must have **Is gateway** enabled. Each sensor uses its
MAC address as its device name. This output uses the ThingsBoard Gateway API.

Run with the file you edited:

```sh
chmod 600 config/thermopro-sensor.local.json
./build/thermopro-sensor --check-config config/thermopro-sensor.local.json
./build/thermopro-sensor config/thermopro-sensor.local.json
```

During an outage, SQLite and JSON continue. MQTT retries automatically, including
after TLS failures, with a delay capped at 30 seconds. It keeps the latest waiting
reading per sensor, up to `max_pending_sensors` (default 256).
Pending readings are lost on exit. SQLite history is not replayed over MQTT.
Check stderr for connection status and errors. Restart after configuration changes.

## Run as a service

Requires systemd 247 or later. Install the binary, sample config, service, and
license with CMake:

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build
sudo cmake --install build
sudo systemctl daemon-reload
sudo systemctl enable --now thermopro-sensor.service
```

Existing config is preserved. New config is installed with permissions `0600`.
For MQTT, edit the installed config before starting the service. Local config
files are never installed automatically.
`CMAKE_INSTALL_BINDIR` and `SYSTEMD_UNIT_DIR` override the binary and service
directories. The default service directory is `lib/systemd/system` under the
install prefix. Packaging can use `DESTDIR=/path/to/staging cmake --install build`.

The service runs from `/var/lib/thermopro-sensor`, where the sample stores
`readings.db`. Edit `/etc/thermopro-sensor/thermopro-sensor.json` for service settings.

```sh
sudo systemctl restart thermopro-sensor.service
sudo journalctl -u thermopro-sensor.service -f -o cat
```

## Tests

```sh
ctest --test-dir build --output-on-failure
```

Optional MQTT and BlueZ integration tests are enabled through
`ENABLE_MQTT_INTEGRATION_TESTS` and `ENABLE_BLUEZ_INTEGRATION_TESTS` in
[CMakeLists.txt](CMakeLists.txt), which lists their dependencies.

Licensed under [GPL-3.0-only](LICENSE).

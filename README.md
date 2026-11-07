# ThermoPro sensor reader

Linux C program that discovers compatible ThermoPro Bluetooth LE sensors,
prints JSON readings, and optionally logs to SQLite and ThingsBoard MQTT.
No sensor addresses or pairing needed. Discovery matches the packet format,
so not every ThermoPro model is supported.

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

During an outage, SQLite and JSON continue. MQTT retries and keeps the latest
waiting reading per sensor, up to `max_pending_sensors` (default 256).
Pending readings are lost on exit. SQLite history is not replayed over MQTT.
Check stderr for disabled, connecting, offline, or error messages. Fix settings
and restart if the MQTT network thread stops.

## Run as a service

Requires systemd 247 or later. For MQTT, use your local config as the source in
the config install command below. That command replaces any installed config.

```sh
sudo install -m 0755 build/thermopro-sensor /usr/local/bin/thermopro-sensor
sudo install -d -m 0755 /etc/thermopro-sensor
sudo install -m 0600 config/thermopro-sensor.json /etc/thermopro-sensor/thermopro-sensor.json
sudo install -m 0644 deploy/thermopro-sensor.service /etc/systemd/system/thermopro-sensor.service
sudo systemctl daemon-reload
sudo systemctl enable --now thermopro-sensor.service
```

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

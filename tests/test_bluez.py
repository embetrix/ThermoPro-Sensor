"""Test the C program with a private D-Bus and a fake BlueZ service

Python and dbus-fast are only needed for this test
"""

import asyncio
import json
import os
from pathlib import Path
import signal
import sqlite3
import sys
import tempfile
from datetime import datetime

from test_mqtt import Broker, CONNECT, TELEMETRY, packet, ack

from dbus_fast import Message, MessageType, Variant
from dbus_fast.aio import MessageBus


ADAPTER = "/org/bluez/hci0"
ADAPTER_IFACE = "org.bluez.Adapter1"
DEVICE_IFACE = "org.bluez.Device1"
PROPERTIES_IFACE = "org.freedesktop.DBus.Properties"
OBJECT_MANAGER_IFACE = "org.freedesktop.DBus.ObjectManager"
FIRST = f"{ADAPTER}/dev_EC_20_1E_58_25_10"
SECOND = f"{ADAPTER}/dev_FA_5B_AB_FB_56_D6"


async def exercise(binary, bus, address, directory, duplicates, protocol, logging_mode=None, fail_write=False):
    payload_length, marker, max_humidity = protocol
    warm_humidity = min(48, max_humidity)
    updated_humidity = min(50, max_humidity)

    def manufacturer(temperature, humidity, battery_ok=True, *, valid_marker=True):
        raw = temperature & 0xFFFF
        key = ((raw & 0xFF) << 8) | (marker if valid_marker else marker ^ 1)
        data = bytes([raw >> 8, humidity, 2 if battery_ok else 0]) + bytes(payload_length - 3)
        return Variant("a{qv}", {key: Variant("ay", data)})

    started = asyncio.Event()
    stopped = asyncio.Event()
    filters = []

    def handle(message):
        if (message.message_type != MessageType.METHOD_CALL
                or message.path != ADAPTER or message.interface != ADAPTER_IFACE):
            return None
        if message.member == "SetDiscoveryFilter":
            filters.append(message.body[0])
        elif message.member == "StartDiscovery":
            started.set()
        elif message.member == "StopDiscovery":
            stopped.set()
        else:
            return None
        return Message.new_method_return(message)

    config = Path(directory) / "thermopro-sensor.json"
    settings = {
        "polling_period_seconds": 1,
        "DuplicateData": duplicates,
        "discover_sensors": True,
    }
    database = Path(directory) / f"{duplicates}-{logging_mode}-{fail_write}.db"
    if logging_mode:
        settings["database"] = {"path": str(database), "logging_mode": logging_mode, "max_size_mb": 10}
    mqtt_broker = None
    mqtt_task = None
    mqtt_rows = asyncio.Queue()
    if duplicates and logging_mode == "per_report" and not fail_write:
        mqtt_broker = await Broker().start()
        settings["mqtt"] = {"host": "127.0.0.1", "port": mqtt_broker.port,
                            "tls": False, "access_token": "test-token"}

    async def collect_mqtt():
        reader, writer = await mqtt_broker.connection()
        registered = set()
        while True:
            header, body = await packet(reader)
            if header >> 4 == 14:
                return
            assert header >> 4 == 3 and (header >> 1) & 3 == 1
            length = int.from_bytes(body[:2], "big")
            topic = body[2:2 + length].decode()
            mid = body[2 + length:4 + length]
            data = json.loads(body[4 + length:])
            if topic == CONNECT:
                registered.add(data["device"])
            else:
                assert topic == TELEMETRY
                for name, entries in data.items():
                    assert name in registered
                    for entry in entries:
                        mqtt_rows.put_nowait((name, entry))
            await ack(writer, mid)

    async def check_mqtt(readings):
        if not mqtt_broker:
            return
        expected = {name: value for name, value in readings.items() if value is not None}
        for _ in expected:
            name, entry = await asyncio.wait_for(mqtt_rows.get(), 3)
            value = expected[name]
            received = datetime.fromisoformat(value["received_at"])
            assert entry["ts"] == int(received.timestamp()) * 1000 + received.microsecond // 1000
            fields = {key: item for key, item in value.items() if key != "received_at" and item is not None}
            assert entry["values"] == fields

    first_name = "EC:20:1E:58:25:10"
    second_name = "FA:5B:AB:FB:56:D6"
    config.write_text(json.dumps(settings))
    bus.add_message_handler(handle)
    process = await asyncio.create_subprocess_exec(
        binary, str(config),
        env={**os.environ, "DBUS_SYSTEM_BUS_ADDRESS": address},
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
    )

    if mqtt_broker:
        mqtt_task = asyncio.create_task(collect_mqtt())

    async def batch():
        rows = []
        for _ in range(2):
            line = await asyncio.wait_for(process.stdout.readline(), timeout=4)
            assert line, "sensor exited before printing output"
            rows.append(json.loads(line))
        return {row["sensor"]: row["reading"] for row in rows}

    async def changed(path, properties, invalidated=None):
        await bus.send(Message.new_signal(
            path, PROPERTIES_IFACE, "PropertiesChanged", "sa{sv}as",
            [DEVICE_IFACE, properties, invalidated or []],
        ))

    try:
        await asyncio.wait_for(started.wait(), timeout=4)
        assert len(filters) == 1
        assert filters[0]["Transport"].value == "le"
        assert filters[0]["DuplicateData"].signature == "b"
        assert filters[0]["DuplicateData"].value is duplicates
        # Add unknown devices only after a matching packet
        unknown = f"{ADAPTER}/dev_AA_BB_CC_DD_EE_FF"
        await changed(unknown, {"RSSI": Variant("n", -42)})
        await changed(unknown, {"ManufacturerData": manufacturer(235, warm_humidity, valid_marker=False)})
        await bus.send(Message.new_signal(
            "/", OBJECT_MANAGER_IFACE, "InterfacesAdded", "oa{sa{sv}}",
            [FIRST, {DEVICE_IFACE: {
                "Address": Variant("s", "EC:20:1E:58:25:10"),
                "ManufacturerData": manufacturer(235, warm_humidity),
                "RSSI": Variant("n", -67),
            }}],
        ))
        await changed(SECOND, {"ManufacturerData": manufacturer(-55, max_humidity, battery_ok=False)})
        readings = await batch()
        await check_mqtt(readings)
        assert readings[first_name]["temperature_c"] == 23.5
        assert readings[first_name]["humidity_pct"] == warm_humidity
        assert readings[first_name]["battery_low"] is False
        assert readings[first_name]["rssi_dbm"] == -67
        assert readings[first_name]["received_at"].endswith("+00:00")
        assert readings[second_name]["temperature_c"] == -5.5
        assert readings[second_name]["battery_low"] is True
        assert readings[second_name]["rssi_dbm"] is None

        # Keep only the latest reading in each report period
        await changed(FIRST, {"ManufacturerData": manufacturer(235, warm_humidity)})
        await changed(FIRST, {"ManufacturerData": manufacturer(235, warm_humidity)})
        await changed(FIRST, {"ManufacturerData": manufacturer(240, updated_humidity)}, ["RSSI"])
        # Ignore invalid packets, other adapters, and RSSI-only updates
        await changed(SECOND, {"ManufacturerData": manufacturer(235, max_humidity + 1)})
        await changed(SECOND, {"RSSI": Variant("n", -80)})
        await changed(SECOND.replace("hci0", "hci1"), {
            "ManufacturerData": manufacturer(235, warm_humidity),
        })
        readings = await batch()
        await check_mqtt(readings)
        assert readings[first_name]["temperature_c"] == 24.0
        assert readings[first_name]["humidity_pct"] == updated_humidity
        assert readings[first_name]["rssi_dbm"] is None
        assert readings[second_name] is None
        # These updates arrive after the report but must not produce new rows
        await changed(FIRST, {"RSSI": Variant("n", -75)})
        await changed(FIRST, {"ManufacturerData": manufacturer(235, warm_humidity, valid_marker=False)})
        assert await batch() == {first_name: None, second_name: None}
        if logging_mode:
            with sqlite3.connect(database) as db:
                rows = db.execute("SELECT sensor_address, received_at, temperature_tenths, humidity_pct, battery_low, rssi_dbm FROM readings ORDER BY id").fetchall()
                assert len(rows) == (3 if logging_mode == "per_report" else 5), rows
                assert rows[0][0] == first_name and rows[0][2:] == (235, warm_humidity, 0, -67)
                assert rows[1][0] == second_name and rows[1][2:] == (-55, max_humidity, 1, None)
                assert rows[-1][2:] == (240, updated_humidity, 0, None)
                assert all(row[1].endswith("+00:00") for row in rows)
                if logging_mode == "every_packet":
                    assert rows[2][2:] == rows[3][2:] == (235, warm_humidity, 0, -67)
                assert db.execute("PRAGMA journal_mode").fetchone()[0] == "wal"
        if fail_write:
            with sqlite3.connect(database) as db:
                db.execute("CREATE TRIGGER reject_readings BEFORE INSERT ON readings BEGIN SELECT RAISE(ABORT, 'test write failure'); END")
            await changed(FIRST, {"ManufacturerData": manufacturer(250, warm_humidity)})
            assert await asyncio.wait_for(process.wait(), timeout=4) != 0
            stderr = (await process.stderr.read()).decode()
            assert "Database error (insert reading)" in stderr, stderr
            assert "test write failure" in stderr, stderr
            with sqlite3.connect(database) as db:
                assert db.execute("SELECT count(*) FROM readings").fetchone()[0] == len(rows)
        else:
            process.send_signal(signal.SIGTERM)
            assert await asyncio.wait_for(process.wait(), timeout=4) == 0
        assert stopped.is_set(), "StopDiscovery was not called on shutdown"
    finally:
        if process.returncode is None:
            process.kill()
            await process.wait()
        stderr = (await process.stderr.read()).decode()
        if process.returncode:
            print(stderr, file=sys.stderr)
        bus.remove_message_handler(handle)
        if mqtt_task:
            mqtt_task.cancel()
            try:
                await mqtt_task
            except (asyncio.CancelledError, asyncio.IncompleteReadError):
                pass
            await mqtt_broker.close()


async def main():
    binary, daemon_binary, *protocol_args = sys.argv[1:]
    protocol = tuple(int(value, 0) for value in protocol_args) if protocol_args else (5, 0xC2, 100)
    with tempfile.TemporaryDirectory(prefix="thermopro-test-") as directory:
        daemon = await asyncio.create_subprocess_exec(
            daemon_binary, "--session", "--nofork", "--print-address=1",
            f"--address=unix:tmpdir={directory}",
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
        bus = None
        try:
            address = (await asyncio.wait_for(daemon.stdout.readline(), timeout=4)).decode().strip()
            if not address:
                raise RuntimeError((await daemon.stderr.read()).decode())
            bus = await MessageBus(bus_address=address).connect()
            await bus.request_name("org.bluez")
            for duplicates in (True, False):
                for logging_mode in (None, "per_report", "every_packet"):
                    await exercise(binary, bus, address, directory, duplicates, protocol, logging_mode)
            for logging_mode in ("per_report", "every_packet"):
                await exercise(binary, bus, address, directory, True, protocol, logging_mode, fail_write=True)
            print("BlueZ tests passed: discovery, DuplicateData, packets, reports, SQLite logging modes, and shutdown")
        finally:
            if bus is not None:
                bus.disconnect()
                await bus.wait_for_disconnect()
            if daemon.returncode is None:
                daemon.terminate()
            await daemon.wait()


if __name__ == "__main__":
    asyncio.run(main())

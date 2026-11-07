"""Check MQTT messages, reconnects, queue limits, and TLS on local sockets"""

import asyncio
import json
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile

FIRST = "EC:20:1E:58:25:10"
SECOND = "FA:5B:AB:FB:56:D6"
CONNECT = "v1/gateway/connect"
TELEMETRY = "v1/gateway/telemetry"


async def packet(reader):
    header = (await reader.readexactly(1))[0]
    size = 0
    multiplier = 1
    for _ in range(4):
        byte = (await reader.readexactly(1))[0]
        size += (byte & 127) * multiplier
        if byte < 128:
            return header, await reader.readexactly(size)
        multiplier *= 128
    raise AssertionError("invalid MQTT length")


class Broker:
    async def start(self, port=0, context=None):
        self.clients = asyncio.Queue()
        self.writers = []
        self.server = await asyncio.start_server(self.accept, "127.0.0.1", port, ssl=context)
        self.port = self.server.sockets[0].getsockname()[1]
        return self

    def accept(self, reader, writer):
        self.writers.append(writer)
        self.clients.put_nowait((reader, writer))

    async def connection(self):
        reader, writer = await asyncio.wait_for(self.clients.get(), 6)
        header, body = await asyncio.wait_for(packet(reader), 3)
        assert header == 0x10 and body[:7] == b"\x00\x04MQTT\x04"
        assert body[7] & 0x80 and not body[7] & 0x40
        offset = 10
        client_length = int.from_bytes(body[offset:offset + 2], "big")
        offset += 2 + client_length
        token_length = int.from_bytes(body[offset:offset + 2], "big")
        assert body[offset + 2:offset + 2 + token_length] == b"test-token"
        writer.write(b"\x20\x02\x00\x00")
        await writer.drain()
        return reader, writer

    async def close(self):
        for writer in self.writers:
            writer.close()
        self.server.close()
        await self.server.wait_closed()


async def publish(reader, expected_topic):
    header, body = await asyncio.wait_for(packet(reader), 4)
    assert header >> 4 == 3 and (header >> 1) & 3 == 1 and not header & 1
    length = int.from_bytes(body[:2], "big")
    assert body[2:2 + length].decode() == expected_topic
    message_id = body[2 + length:4 + length]
    return message_id, json.loads(body[4 + length:])


async def ack(writer, message_id):
    writer.write(b"\x40\x02" + message_id)
    await writer.drain()


async def quiet(reader):
    try:
        await asyncio.wait_for(reader.readexactly(1), 0.15)
    except asyncio.TimeoutError:
        return
    raise AssertionError("unexpected MQTT message")


async def client(binary, directory, port, **options):
    config = Path(directory) / "mqtt.json"
    config.write_text(json.dumps({"polling_period_seconds": 1, "mqtt": {
        "host": "127.0.0.1", "port": port, "tls": False,
        "access_token": "test-token", **options,
    }}))
    process = await asyncio.create_subprocess_exec(
        binary, str(config), stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
    )
    assert await asyncio.wait_for(process.stdout.readline(), 3) == b"ready\n"
    return process


async def reading(process, address=FIRST, temperature=133, timestamp=1794046826202, fresh=1, rssi=-67):
    process.stdin.write(f"{address} {temperature} {timestamp} {fresh} {rssi}\n".encode())
    await process.stdin.drain()
    assert await asyncio.wait_for(process.stdout.readline(), 2) == b"queued\n"


async def stop(process):
    process.stdin.close()
    try:
        assert await asyncio.wait_for(process.wait(), 4) == 0
    finally:
        if process.returncode is None:
            process.kill()
            await process.wait()
    errors = (await process.stderr.read()).decode()
    assert "test-token" not in errors
    return errors


async def test_messages(binary, directory):
    broker = await Broker().start()
    process = await client(binary, directory, broker.port)
    try:
        reader, writer = await broker.connection()
        await reading(process)
        mid, data = await publish(reader, CONNECT)
        assert data == {"device": FIRST}
        await reading(process, temperature=240, timestamp=1794046826300, rssi=0)
        await quiet(reader)
        await ack(writer, mid)
        mid, data = await publish(reader, TELEMETRY)
        assert data == {FIRST: [{"ts": 1794046826300, "values": {
            "temperature_c": 24.0, "humidity_pct": 48, "battery_low": True,
        }}]}
        await ack(writer, mid)
        await reading(process, fresh=0)
        await quiet(reader)
        await reading(process, address=SECOND, temperature=-55)
        mid, data = await publish(reader, CONNECT)
        assert data == {"device": SECOND}
        await ack(writer, mid)
        mid, data = await publish(reader, TELEMETRY)
        assert data[SECOND][0]["values"]["temperature_c"] == -5.5
        assert data[SECOND][0]["values"]["rssi_dbm"] == -67
        # Drop the connection before PUBACK, then check the QoS 1 retry
        writer.close()
        await writer.wait_closed()
        reader, writer = await broker.connection()
        retry_mid, retry = await publish(reader, TELEMETRY)
        assert retry == data and retry_mid == mid
        await ack(writer, retry_mid)
        await reading(process)
        mid, data = await publish(reader, CONNECT)
        assert data == {"device": FIRST}
        await ack(writer, mid)
        mid, data = await publish(reader, TELEMETRY)
        await ack(writer, mid)
    finally:
        await stop(process)
        await broker.close()


async def test_offline(binary, directory):
    with socket.socket() as reserved:
        reserved.bind(("127.0.0.1", 0))
        port = reserved.getsockname()[1]
    process = await client(binary, directory, port, max_pending_sensors=1)
    broker = None
    try:
        message = await asyncio.wait_for(process.stderr.readline(), 3)
        assert message == f"MQTT connecting to 127.0.0.1:{port}\n".encode()
        message = await asyncio.wait_for(process.stderr.readline(), 3)
        assert b"connection failed, retrying" in message and b"Connection refused" in message
        assert b"test-token" not in message
        await reading(process, temperature=133)
        await reading(process, temperature=250)
        await reading(process, address=SECOND)
        broker = await Broker().start(port)
        reader, writer = await broker.connection()
        mid, data = await publish(reader, CONNECT)
        assert data == {"device": FIRST}
        await ack(writer, mid)
        mid, data = await publish(reader, TELEMETRY)
        assert data[FIRST][0]["values"]["temperature_c"] == 25.0
        await ack(writer, mid)
        await quiet(reader)
    finally:
        errors = await stop(process)
        if broker:
            await broker.close()
    assert "MQTT queue full" in errors


async def test_pending_connection(binary, directory):
    broker = await Broker().start()
    process = await client(binary, directory, broker.port)
    try:
        reader, writer = await asyncio.wait_for(broker.clients.get(), 3)
        header, _ = await asyncio.wait_for(packet(reader), 3)
        assert header == 0x10
        message = await asyncio.wait_for(process.stderr.readline(), 3)
        assert b"MQTT connecting to" in message
        # A broker that never sends CONNACK must not leave the user without status
        await asyncio.sleep(31)
        await reading(process, fresh=0)
        message = await asyncio.wait_for(process.stderr.readline(), 3)
        assert b"MQTT offline" in message and b"0 sensors waiting" in message
        assert b"waiting for connection or retry" in message and b"test-token" not in message
        writer.write(b"\x20\x02\x00\x00")
        await writer.drain()
        message = await asyncio.wait_for(process.stderr.readline(), 3)
        assert b"MQTT connected" in message
        await reading(process)
        mid, _ = await publish(reader, CONNECT)
        await ack(writer, mid)
        mid, _ = await publish(reader, TELEMETRY)
        await ack(writer, mid)
    finally:
        await stop(process)
        await broker.close()


async def test_tls(binary, openssl, directory):
    cert = Path(directory) / "cert.pem"
    key = Path(directory) / "key.pem"
    subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-keyout", str(key), "-out", str(cert), "-subj", "/CN=mqtt-test.invalid",
                    "-addext", "subjectAltName=IP:127.0.0.1"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    broker = await Broker().start(context=context)
    process = await client(binary, directory, broker.port, tls=True, ca_file=str(cert))
    try:
        reader, writer = await broker.connection()
        await reading(process)
        mid, _ = await publish(reader, CONNECT)
        await ack(writer, mid)
        mid, _ = await publish(reader, TELEMETRY)
        await ack(writer, mid)
    finally:
        await stop(process)
        await broker.close()
    # Neither an untrusted CA nor a wrong hostname may receive credentials
    for options in ({}, {"host": "localhost", "ca_file": str(cert)}):
        broker = await Broker().start(context=context)
        process = await client(binary, directory, broker.port, tls=True, **options)
        try:
            await reading(process)
            try:
                reader, _ = await asyncio.wait_for(broker.clients.get(), 1)
                assert await asyncio.wait_for(reader.read(), 1) == b""
            except asyncio.TimeoutError:
                pass
        finally:
            await stop(process)
            await broker.close()


async def main():
    binary, openssl = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="thermopro-mqtt-") as directory:
        await test_messages(binary, directory)
        await test_offline(binary, directory)
        await test_pending_connection(binary, directory)
        await test_tls(binary, openssl, directory)
    print("MQTT messages, reconnects, offline queue, and TLS tests passed")


if __name__ == "__main__":
    asyncio.run(main())

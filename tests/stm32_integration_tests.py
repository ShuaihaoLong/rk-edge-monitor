#!/usr/bin/env python3
"""使用 PTY 和本地 MQTT 测试端验证正式应用的 STM32 双向数据路径。"""
import json
import os
import pathlib
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib


def exact(sock, size):
    value = b""
    while len(value) < size:
        part = sock.recv(size - len(value))
        if not part:
            raise EOFError()
        value += part
    return value


def packet(sock):
    header = exact(sock, 1)[0]
    length, scale = 0, 1
    for _ in range(4):
        byte = exact(sock, 1)[0]
        length += (byte & 127) * scale
        if not byte & 128:
            return header, exact(sock, length)
        scale *= 128
    raise AssertionError("malformed MQTT length")


def mqtt_packet(header, body):
    size, encoded = len(body), bytearray([header])
    while True:
        byte, size = size % 128, size // 128
        encoded.append(byte | (128 if size else 0))
        if not size:
            return bytes(encoded) + body


def mqtt_string(value):
    return struct.pack(">H", len(value)) + value


def frame(kind, seq, payload):
    body = struct.pack("<BBIH", 1, kind, seq, len(payload)) + payload
    return b"\xaa\x55" + body + struct.pack("<I", zlib.crc32(body))


class Broker:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.send_lock = threading.Lock()
        self.connections, self.threads, self.messages, self.errors = [], [], [], []
        self.bridge = None
        self.subscriptions = self.pings = 0
        self.worker = threading.Thread(target=self.accept)
        self.worker.start()

    def accept(self):
        while not self.stop.is_set():
            try:
                conn, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.connections.append(conn)
            thread = threading.Thread(target=self.serve, args=(conn,))
            self.threads.append(thread)
            thread.start()

    def serve(self, conn):
        try:
            header, body = packet(conn)
            assert header == 0x10
            bridge = b"-stm32" in body
            if bridge:
                assert b"rkmon/devices/integration/stm32/status" in body
                assert b'"online":false' in body
            conn.sendall(b"\x20\x02\x00\x00")
            while not self.stop.is_set():
                header, body = packet(conn)
                if header == 0x82:
                    assert bridge and body[0:2] == b"\x00\x01" and body[-1] == 0
                    assert body[4:-1] == b"rkmon/devices/integration/stm32/command"
                    # TCP 分片确认，避免实现假定一次 recv 就有完整报文。
                    with self.send_lock:
                        conn.sendall(b"\x90")
                        time.sleep(0.01)
                        conn.sendall(b"\x03\x00\x01\x00")
                    with self.lock:
                        self.bridge = conn
                        self.subscriptions += 1
                elif header in (0x30, 0x31):
                    length = struct.unpack(">H", body[:2])[0]
                    topic = body[2:2 + length].decode()
                    payload = json.loads(body[2 + length:])
                    with self.lock:
                        self.messages.append((topic, header, payload))
                elif header == 0xc0:
                    assert body == b""
                    with self.send_lock:
                        conn.sendall(b"\xd0\x00")
                    with self.lock:
                        self.pings += 1
                elif header == 0xe0:
                    return
                else:
                    raise AssertionError(f"unexpected packet {header:#x}")
        except (EOFError, OSError):
            pass
        except Exception as error:
            self.errors.append(error)

    def publish_command(self, payload, retained=False):
        with self.lock:
            bridge = self.bridge
        body = mqtt_string(b"rkmon/devices/integration/stm32/command") + payload
        wire = mqtt_packet(0x31 if retained else 0x30, body)
        with self.send_lock:
            bridge.sendall(wire[:2])
            time.sleep(0.01)
            bridge.sendall(wire[2:])

    def has(self, suffix, predicate=lambda value: True, since=0):
        with self.lock:
            return any(topic.endswith("/stm32/" + suffix) and predicate(value)
                       for topic, _, value in self.messages[since:])

    def close(self):
        self.stop.set()
        self.worker.join(1)
        self.listener.close()
        for conn in self.connections:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            conn.close()
        for thread in self.threads:
            thread.join(1)


def wait_for(predicate, timeout=5):
    end = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= end:
            raise AssertionError("condition timed out")
        time.sleep(0.02)


def read_frame(master):
    data = b""
    end = time.monotonic() + 3
    while time.monotonic() < end:
        if select.select([master], [], [], 0.1)[0]:
            data += os.read(master, 4096)
            if len(data) >= 10:
                size = 14 + struct.unpack_from("<H", data, 8)[0]
                if len(data) >= size:
                    assert len(data) == size and data[:2] == b"\xaa\x55"
                    assert zlib.crc32(data[2:-4]) == struct.unpack("<I", data[-4:])[0]
                    return data
    raise AssertionError("serial command missing")


def main():
    broker = Broker()
    master, slave = os.openpty()
    process = None
    try:
        with tempfile.TemporaryDirectory(prefix="rkmon-stm32-") as directory:
            config = pathlib.Path(directory) / "test.ini"
            serial_path = pathlib.Path(directory) / "uart"
            config.write_text(f"""[logging]
file=
[mqtt]
enabled=true
device_id=integration
broker_port={broker.port}
publish_interval_ms=1000
keepalive_seconds=2
[stm32]
enabled=true
device={serial_path}
stale_timeout_ms=1500
reconnect_interval_ms=100
""")
            with open(pathlib.Path(directory) / "app.log", "w+") as log:
                process = subprocess.Popen([sys.argv[1], "--config", str(config)], stdout=log, stderr=log)
                try:
                    wait_for(lambda: broker.subscriptions == 1)
                    serial_path.symlink_to(os.ttyname(slave))
                    time.sleep(0.25)
                    wire = frame(0x10, 362, bytes([28, 5, 47, 0]))
                    os.write(master, b"noise" + wire[:7])
                    time.sleep(0.02)
                    os.write(master, wire[7:])
                    wait_for(lambda: broker.has("telemetry", lambda p: p["temperature_c"] == 28.5 and p["humidity_percent"] == 47))
                    broker.publish_command(b"\x01\x00\xff", retained=True)
                    wait_for(lambda: broker.has("command_result", lambda p: p["state"] == "rejected_retained"))
                    broker.publish_command(b"x" * 129)
                    wait_for(lambda: broker.has("command_result", lambda p: p["state"] == "rejected_length"))
                    broker.publish_command(b"\x01\x00\xff")
                    command = read_frame(master)
                    assert command[3] == 0x20 and command[10:-4] == b"\x01\x00\xff"
                    seq = struct.unpack_from("<I", command, 4)[0]
                    wait_for(lambda: broker.has("command_result", lambda p: p["state"] == "sent" and p["sequence"] == seq))
                    os.write(master, frame(0, 999, b"\x02\xff"))
                    wait_for(lambda: broker.has("ack", lambda p: p["sequence"] == 999 and p["payload_hex"] == "02ff"))
                    wait_for(lambda: broker.has("status", lambda p: p["online"] is True))
                    marker = len(broker.messages)
                    bad = bytearray(wire); bad[-1] ^= 1
                    os.write(master, bad + frame(0x10, 363, b"\x1c\x05"))
                    wait_for(lambda: broker.has("status", lambda p: p["online"] is False, marker))
                    assert not broker.has("telemetry", since=marker)
                    broker.publish_command(b"\x01")
                    wait_for(lambda: broker.has("command_result", lambda p: p["state"] == "rejected_offline"))
                    assert not select.select([master], [], [], 0.1)[0]
                    os.write(master, frame(0x10, 364, bytes([29, 0, 46, 5])))
                    wait_for(lambda: broker.has("telemetry", lambda p: p["sequence"] == 364))
                    with broker.lock:
                        broker.bridge.shutdown(socket.SHUT_RDWR)
                    wait_for(lambda: broker.subscriptions == 2)
                    os.write(master, frame(0x10, 365, bytes([29, 1, 46, 0])))
                    wait_for(lambda: broker.has("telemetry", lambda p: p["sequence"] == 365))
                    new_master, new_slave = os.openpty()
                    serial_path.unlink()
                    serial_path.symlink_to(os.ttyname(new_slave))
                    os.close(master); os.close(slave)
                    master, slave = new_master, new_slave
                    time.sleep(0.3)
                    os.write(master, frame(0x10, 1, bytes([26, 5, 50, 0])))
                    wait_for(lambda: broker.has("telemetry", lambda p: p["sequence"] == 1 and p["temperature_c"] == 26.5))
                    wait_for(lambda: broker.pings > 0)
                    process.send_signal(signal.SIGTERM)
                    assert process.wait(timeout=5) == 0
                    wait_for(lambda: broker.has("status", lambda p: not p["serial_connected"]))
                    assert not broker.errors, broker.errors
                    assert all(header == (0x31 if topic.endswith("/status") else 0x30)
                               for topic, header, _ in broker.messages)
                    print("STM32 PTY/MQTT telemetry, CRC rejection, commands, raw ACK, stale data, reconnect and shutdown passed")
                except Exception:
                    log.flush(); log.seek(0)
                    print(log.read(), file=sys.stderr)
                    raise
    finally:
        if process is not None and process.poll() is None:
            process.kill(); process.wait()
        os.close(master); os.close(slave)
        broker.close()


if __name__ == "__main__":
    main()

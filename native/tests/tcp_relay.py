"""Real loopback sockets; only local synthetic endpoints, no driver/firewall changes."""
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def exact(sock, count):
    result = b""
    while len(result) < count:
        chunk = sock.recv(count - len(result))
        assert chunk, (len(result), count)
        result += chunk
    return result


def hello(up=True, mode=3):
    return (bytes.fromhex("3366000b000c") + struct.pack("!H B III", 0x1001 if up else 0x1002,
            0 if up else 1, 1, 88, 0) + bytes([mode, 0, 64]) + (2 if up else 4).to_bytes(64, "big"))


def events(path):
    if not path.exists():
        return []
    # The last record can still be being written by the process.
    return [json.loads(s) for s in path.read_text(encoding="utf-8").split('\n')[:-1]]


def main():
    with tempfile.TemporaryDirectory(prefix="radaer-tcp-") as folder, socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(64)
        listener.settimeout(4)
        output = Path(folder) / "relay.jsonl"
        proc = subprocess.Popen([sys.argv[1], "capture", "--relay", "true", "--listen", "127.0.0.1:0",
            "--upstream", f"127.0.0.1:{listener.getsockname()[1]}", "--out", str(output),
            "--device", "loopback", "--synthetic", "true", "--seconds", "8"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        pairs = []
        try:
            deadline = time.monotonic() + 5
            while True:
                bound = next((e["data"] for e in events(output) if e["type"] == "tcp_listener_started"), None)
                if bound:
                    break
                assert proc.poll() is None and time.monotonic() < deadline
                time.sleep(.02)
            port = int(bound["listen"].rsplit(":", 1)[1])
            assert port > 0 and bound["automaticPort"]

            def pair():
                client = socket.create_connection(("127.0.0.1", port), timeout=4)
                server, _ = listener.accept()
                server.settimeout(4)
                pairs.append((client, server))
                return client, server

            for payload in (b"GET / HTTP/1.1\r\n\r\n", bytes.fromhex("16030300050102030405"), hello(mode=2)):
                client, server = pair()
                client.sendall(payload)
                assert exact(server, len(payload)) == payload
                reply = bytes(range(256)) * 512
                server.sendall(reply)
                assert exact(client, len(reply)) == reply
                client.close(); server.close()

            client, server = pair()
            prefix = hello()[:15]
            client.sendall(prefix)
            banner = hello(False)[:6]  # A server-first TGCP-like prefix must also pass through.
            server.sendall(banner)
            assert exact(client, len(banner)) == banner
            assert exact(server, len(prefix)) == prefix
            client.close(); server.close()

            client, server = pair()
            start = time.monotonic()
            client.sendall(prefix)
            assert exact(server, len(prefix)) == prefix
            assert .7 <= time.monotonic() - start < 4
            suffix = hello()[15:]
            client.sendall(suffix)
            assert exact(server, len(suffix)) == suffix
            client.close(); server.close()

            client, server = pair()
            client.sendall(prefix); client.shutdown(socket.SHUT_WR)
            assert exact(server, len(prefix)) == prefix and server.recv(1) == b""
            server.sendall(b"reply after half close"); server.shutdown(socket.SHUT_WR)
            assert exact(client, 22) == b"reply after half close" and client.recv(1) == b""
            client.close(); server.close()

            client, server = pair()
            client.sendall(hello()[:15]); client.sendall(hello()[15:])
            rewritten = exact(server, 88)
            assert rewritten[:24] == hello()[:24] and rewritten[24:] != hello()[24:]
            server.sendall(hello(False))
            reply = exact(client, 88)
            assert reply[:24] == hello(False)[:24] and reply[24:] != hello(False)[24:]
            client.close(); server.close()

            client, server = pair()
            client.sendall(hello()); exact(server, 88)
            server.sendall(b"invalid response after DH substitution")
            try:
                assert client.recv(1) == b""
            except ConnectionResetError:
                pass
            client.close(); server.close()

            # Full-port capture must support ordinary browsers' concurrent connections.
            concurrent = [pair() for _ in range(24)]
            for i, (client, server) in enumerate(concurrent):
                payload = b"HTTP synthetic " + str(i).encode()
                client.sendall(payload)
                assert exact(server, len(payload)) == payload
                server.sendall(payload)
                assert exact(client, len(payload)) == payload
            for client, server in concurrent:
                client.close(); server.close()
            stdout, stderr = proc.communicate(timeout=12)
            assert proc.returncode == 0, stderr.decode(errors="replace")
            records = events(output)
            errors = [e["data"] for e in records if e["type"] == "tcp_error"]
            assert len(errors) == 1 and errors[0]["dhCommitted"], errors
            assert not any(e["type"] == "tcp_rejected" for e in records)
            reasons = {e["data"]["reason"] for e in records if e["type"] == "tcp_passthrough"}
            assert {"non_tgcp", "server_first", "prefix_timeout", "client_eof"} <= reasons, reasons
            assert sum(e["type"] == "dh_established" for e in records) == 1
            print(json.dumps({"socketConnections": 32, "concurrentConnections": 24,
                "dynamicListener": True, "passthroughReasons": sorted(reasons),
                "fragmentedDh": True, "committedFailureClosed": True}))
        finally:
            for client, server in pairs:
                client.close(); server.close()
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
    return 0


if __name__ == "__main__":
    sys.exit(main())

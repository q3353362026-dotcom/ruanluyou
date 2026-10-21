"""Exercise Windows capture workers with a fake DLL; no kernel driver or networking."""
import ipaddress
import base64
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
from integration import crypt_udp
from player_fixtures import player_records

def main():
    exe, fixture = map(Path, sys.argv[1:])
    result = subprocess.run([str(exe), "network-info"], capture_output=True, encoding="utf-8", check=True)
    inventory = json.loads(result.stdout)
    if not inventory["elevated"]:
        print("SKIP: capture preflight needs elevation even with a mock DLL")
        return 77
    chosen = None
    local_ips = {x["address"] for a in inventory["adapters"] for x in a["ipv4"]}
    for adapter in inventory["adapters"]:
        for address in adapter["ipv4"]:
            host = ipaddress.ip_address(address["address"])
            net = ipaddress.ip_network(f"{host}/{address['prefixLength']}", strict=False)
            if not adapter["up"] or host.is_loopback or host.is_link_local or net.num_addresses < 8:
                continue
            client = net.network_address + 2
            if str(client) in local_ips:
                client += 1
            if str(client) not in local_ips:
                chosen = adapter["ifIndex"], str(host), str(client)
                break
        if chosen:
            break
    if not chosen:
        print("SKIP: no usable adapter for preflight")
        return 77
    index, host, client = chosen
    bank = bytes((i * 37 + 29) & 255 for i in range(128))
    with tempfile.TemporaryDirectory(prefix="radaer-mock-") as folder:
        root = Path(folder)
        dll = root / "WinDivert.dll"
        shutil.copyfile(fixture, dll)
        (root / "WinDivert64.sys").write_bytes(b"TEST FIXTURE; NOT A DRIVER")
        bankfile = root / "bank.bin"
        bankfile.write_bytes(bank)
        source = root / "packets.bin"
        packets = []
        for i, record in enumerate(player_records(496)):
            plain = base64.b64decode(record["data"]["payloadBase64"])
            payload = crypt_udp(plain, bank)
            transport = struct.pack("!HHHH", 22102, 40123, 8 + len(payload), 0) + payload
            ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(transport), i, 0, 64, 17, 0,
                             socket.inet_aton("198.51.100.7"), socket.inet_aton(client))
            packets.append(ip + transport)
        source.write_bytes(b"".join(struct.pack("<I", len(p)) + p for p in packets))
        env = dict(os.environ, RADAER_MOCK_PACKETS=str(source))
        base = [str(exe), "capture", "--shared-windows", "true", "--shared-if-index", str(index),
                "--shared-host-ip", host, "--client-ip", client, "--windivert-dll", str(dll),
                "--seconds", "1", "--device", "mock-client", "--synthetic", "true", str(bankfile)]
        output = root / "capture.jsonl"
        completed = subprocess.run([*base, "--out", str(output)], env=env, capture_output=True, timeout=10)
        assert completed.returncode == 0, completed.stderr
        events = [json.loads(line) for line in output.read_text(encoding="utf-8").splitlines()]
        raw = [e for e in events if e["type"] == "udp_raw"]
        plain = [e for e in events if e["type"] == "udp_plaintext"]
        assert len(raw) == len(plain) == 500, (len(raw), len(plain))
        players = [e for e in events if e["type"] == "player_sample"]
        assert len(players) == 992, len(players)
        assert all(e["data"]["name"] == "测试玩家" for e in players if e["data"]["slot"] == 17)
        assert next(e["data"] for e in events if e["type"] == "player_decoder_summary")["errors"] == 0
        assert all(e["synthetic"] for e in events)
        assert all(e["data"]["timestampSource"] == "WinDivert_QPC" for e in raw)
        assert all(e["data"]["direction"] == "down" for e in raw)
        assert [e["data"]["packetOrdinal"] for e in raw] == list(range(1, 501))
        assert events[-1]["type"] == "session_stopped"
        assert not [e for e in events if e["type"] == "capture_gap"]
        # Split every encrypted datagram below UDP, reverse fragment order and
        # duplicate one fragment. The real worker must restore the original bytes.
        def fragments(packet):
            body = packet[20:]
            cut = max(8, (len(body) // 2 // 8) * 8)
            result = []
            for offset, payload, more in [(0, body[:cut], True), (cut, body[cut:], False)]:
                header = bytearray(packet[:20])
                struct.pack_into('!H', header, 2, 20 + len(payload))
                struct.pack_into('!H', header, 6, (0x2000 if more else 0) | offset // 8)
                result.append(bytes(header) + payload)
            return [result[1], result[1], result[0], result[1]]
        split_source = root / 'fragments.bin'
        split_source.write_bytes(b''.join(struct.pack('<I', len(p)) + p for packet in packets for p in fragments(packet)))
        split_output = root / 'reassembled.jsonl'
        split_env = dict(env, RADAER_MOCK_PACKETS=str(split_source))
        completed = subprocess.run([*base, '--out', str(split_output)], env=split_env, capture_output=True, timeout=10)
        assert completed.returncode == 0, completed.stderr
        split_events = [json.loads(line) for line in split_output.read_text(encoding='utf8').splitlines()]
        split_raw = [e for e in split_events if e['type'] == 'udp_raw']
        split_plain = [e for e in split_events if e['type'] == 'udp_plaintext']
        split_players = [e for e in split_events if e['type'] == 'player_sample']
        assert len(split_raw) == len(split_plain) == 500 and len(split_players) == 992
        assert [e['data']['payloadBase64'] for e in split_raw] == [e['data']['payloadBase64'] for e in raw]
        assert [e['data']['payloadBase64'] for e in split_plain] == [e['data']['payloadBase64'] for e in plain]
        pose = lambda rows: [(e['data']['slot'],e['data']['x'],e['data']['y'],e['data']['z'],e['data']['yaw'],e['data']['teamId']) for e in rows]
        assert pose(split_players) == pose(players)
        assert all(e['data']['ipFragmentCount'] == 2 for e in split_raw + split_plain)
        summary = next(e['data'] for e in split_events if e['type'] == 'udp_reassembly_summary')
        assert summary['ipv4FragmentsReceived'] == 2000 and summary['ipv4DatagramsReassembled'] == 500, summary
        assert summary['ipv4DuplicateFragments'] == 1000 and summary['ipv4ReassemblyPending'] == 0, summary
        assert summary['ipv4ReassemblyRejected'] == summary['ipv4ReassemblyTimeouts'] == 0, summary
        assert not [e for e in split_events if e['type'] == 'capture_gap']
        # No later traffic: expiry must still run while the driver receive blocks.
        incomplete = fragments(packets[0])[0]
        split_source.write_bytes(struct.pack('<I', len(incomplete)) + incomplete)
        incomplete_output = root / 'incomplete.jsonl'
        slow_base = list(base); slow_base[slow_base.index('--seconds') + 1] = '5.5'
        completed = subprocess.run([*slow_base, '--out', str(incomplete_output)], env=split_env, capture_output=True, timeout=10)
        assert completed.returncode == 0, completed.stderr
        incomplete_events = [json.loads(line) for line in incomplete_output.read_text(encoding='utf8').splitlines()]
        summary = next(e['data'] for e in incomplete_events if e['type'] == 'udp_reassembly_summary')
        assert summary['ipv4ReassemblyTimeouts'] == 1 and summary['ipv4ReassemblyPending'] == 0, summary
        assert not any(e['type'] in ('udp_raw', 'udp_plaintext', 'player_sample') for e in incomplete_events)
        assert any(e['type'] == 'capture_gap' for e in incomplete_events)
        failed = root / "failure.jsonl"
        env["RADAER_MOCK_RECEIVE_ERROR"] = "1"
        completed = subprocess.run([*base, "--out", str(failed)], env=env, capture_output=True, timeout=10)
        assert completed.returncode != 0 and b"receive failed" in completed.stderr
        events = [json.loads(line) for line in failed.read_text(encoding="utf-8").splitlines()]
        assert any(e["type"] == "capture_gap" and e["data"]["reason"] == "shared_driver_error" for e in events)
        env.pop("RADAER_MOCK_RECEIVE_ERROR")
        def tcp(src, dst, sp, dp, flags=2):
            transport = struct.pack("!HHIIBBHHH", sp, dp, 12345, 0, 0x50, flags, 8192, 0, 0)
            return struct.pack("!BBHHHBBH4s4s", 0x45, 0, 40, 1, 0x4000, 64, 6, 0,
                               socket.inet_aton(src), socket.inet_aton(dst)) + transport
        tcp_source = root / "tcp.bin"
        sent = root / "sent.bin"
        filters = root / "filters.txt"
        servers = [("198.51.100.7", 80), ("198.51.100.7", 443), ("198.51.100.8", 65535)]
        peer = str(net.network_address + 4)
        assert peer not in (client, host)
        bypassed = [tcp(client, peer, 40123, 80), tcp(peer, client, 80, 40123, 0x12),
                    tcp(client, "198.51.100.9", 40125, 443, 0x10)]
        originals = [tcp(client, server, 40123, port) for server, port in servers]
        # The original remote response must be suppressed for an already redirected route.
        input_tcp = originals + bypassed + [tcp(servers[0][0], client, 80, 40123, 0x12)]
        tcp_source.write_bytes(b"".join(struct.pack("<I", len(p)) + p for p in input_tcp))
        env.update(RADAER_MOCK_TCP=str(tcp_source), RADAER_MOCK_SENT=str(sent), RADAER_MOCK_FILTERS=str(filters))
        automatic_output = root / "automatic.jsonl"
        completed = subprocess.run([*base, "--shared-redirect", "true", "--tcp-port", "0", "--manage-firewall", "false",
                                    "--out", str(automatic_output)], env=env, capture_output=True, timeout=10)
        assert completed.returncode == 0, completed.stderr
        events = [json.loads(line) for line in automatic_output.read_text(encoding="utf-8").splitlines()]
        listener = next(e["data"] for e in events if e["type"] == "tcp_listener_started")
        port = int(listener["listen"].rsplit(":", 1)[1])
        assert port > 0 and listener["automaticPort"] and listener["tcpMode"] == "automatic_all_ports"
        started = next(e["data"] for e in events if e["type"] == "shared_capture_started")
        assert started["listen"] == listener["listen"]
        assert f"tcp.SrcPort == {port}" in started["filters"]["tcpReturn"]
        assert "tcp.DstPort" not in started["filters"]["tcpForward"]
        opened = filters.read_text().splitlines()
        assert len(opened) == 3 and opened[0].startswith("0 0 ")
        assert f"tcp.SrcPort == {port}" in opened[0]
        wire = sent.read_bytes(); records = []
        while wire:
            length = struct.unpack_from("<I", wire)[0]
            meta = struct.unpack_from("<I", wire, 12)[0]
            records.append((meta & 255, bool(meta & (1 << 17)), wire[84:84+length]))
            wire = wire[84+length:]
        up = [p for layer, outbound, p in records if layer == 0 and not outbound]
        down = [p for layer, outbound, p in records if layer == 0 and outbound]
        untouched = [p for layer, outbound, p in records if layer == 1]
        assert len(up) == len(down) == 3 and untouched == bypassed, records
        aliases = {struct.unpack_from("!H", p, 20)[0] for p in up}
        assert len(aliases) == 3, "same client port to distinct targets needs independent aliases"
        assert all(socket.inet_ntoa(p[16:20]) == host and struct.unpack_from("!H", p, 22)[0] == port for p in up)
        restored = {(socket.inet_ntoa(p[12:16]), struct.unpack_from("!H", p, 20)[0]) for p in down}
        assert restored == set(servers)
        assert all(socket.inet_ntoa(p[16:20]) == client and struct.unpack_from("!H", p, 22)[0] == 40123 for p in down)
        assert events[-1]["type"] == "session_stopped"
        print(json.dumps({"syntheticCapturedAndDecrypted": 500, "nativePlayerSamples":992,
                          "fragmentCopies":2000, "reassembledDatagrams":500, "reassembledPlayerSamples":992,
                          "silentFragmentTimeoutVerified":True,
                          "gracefulStop": True,
                          "receiveFailureRecorded": True, "automaticTcpRoutes": 3, "localAndExistingBypassed": 3,
                          "dynamicListenerPort": port, "firewallChanged": False, "realDriverOpened": False}))
    return 0

if __name__ == "__main__":
    sys.exit(main())

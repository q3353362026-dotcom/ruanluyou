"""Read-only WinDivert helpers and preflight. Never opens driver/capture handles."""
import ctypes as ct
import ipaddress
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys

def run(exe, *args, ok=True):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, encoding="utf-8")
    assert (result.returncode == 0) == ok, result.stdout + result.stderr
    return json.loads(result.stdout) if ok else result.stderr

def checksum(raw):
    raw += b"\0" * (len(raw) % 2)
    total = sum(struct.unpack(f"!{len(raw)//2}H", raw))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535

def packet(src, dst, sp=40123, dp=22102, protocol=6):
    transport = (struct.pack("!HHIIBBHHH", sp, dp, 12345, 67890, 0x50, 0x12, 8192, 0, 0)
                 if protocol == 6 else struct.pack("!HHHH", sp, dp, 13, 0))
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(transport) + 5, 77, 0x4000, 64,
                     protocol, 0, socket.inet_aton(src), socket.inet_aton(dst))
    return ip + transport + b"hello"

def main():
    exe, dll = map(Path, sys.argv[1:])
    inventory = run(exe, "network-info")
    selected = None
    for adapter in inventory["adapters"]:
        if not adapter["up"]:
            continue
        for address in adapter["ipv4"]:
            host = ipaddress.ip_address(address["address"])
            network = ipaddress.ip_network(f"{host}/{address['prefixLength']}", strict=False)
            if host.is_loopback or network.num_addresses < 4 or host.is_link_local:
                continue
            candidate = network.network_address + 1
            if candidate == host:
                candidate += 1
            selected = adapter["ifIndex"], str(host), str(candidate)
            break
        if selected:
            break
    if not selected:
        print("SKIP: no active usable IPv4 adapter for read-only preflight")
        return 77
    index, host, client = selected
    common = ["--shared-if-index", str(index), "--shared-host-ip", host,
              "--client-ip", client, "--windivert-dll", str(dll.resolve())]
    passive = run(exe, "shared-preflight", *common)
    assert passive["configurationValid"] and not passive["driverOpened"]
    assert not passive["sharingConnectivityVerified"] and not passive["firewallRuleRequired"]
    active = run(exe, "shared-preflight", *common, "--shared-redirect", "true", "--tcp-port", "22102",
                 "--tcp-server", "198.51.100.7", "--listen", host + ":19001")
    assert active["firewallRuleRequired"] and not active["driverOpened"]
    fixed = run(exe, "shared-preflight", *common, "--shared-redirect", "true")
    assert fixed["tcpMode"] == "filtered" and fixed["returnFilterPendingBind"]
    automatic = run(exe, "shared-preflight", *common, "--shared-redirect", "true", "--tcp-port", "0")
    assert automatic["tcpMode"] == "automatic_all_ports"
    assert automatic["returnFilterPendingBind"] and automatic["filters"]["tcpReturn"] is None
    assert automatic["listen"] == host + ":0" and automatic["firewallManaged"]
    invalid = [
        ["--shared-redirect", "true", "--listen", host + ":0junk"],
        ["--shared-redirect", "true", "--tcp-port", "22102", "--listen", "0.0.0.0:19001"],
        ["--client-ip", host], ["--shared-if-index", "4294967295"],
        ["--tcp-port", "22102"], ["--udp-port", "65536"], ["--shared-windows", "false"],
        ["--windivert-dll", "WinDivert.dll"], ["--upstream", "198.51.100.7:22102"],
    ]
    for options in invalid:
        run(exe, "shared-preflight", *common, *options, ok=False)
    library = ct.CDLL(str(dll.resolve()))
    calc = library.WinDivertHelperCalcChecksums
    calc.argtypes = [ct.c_void_p, ct.c_uint, ct.c_void_p, ct.c_uint64]
    calc.restype = ct.c_int
    evaluate = library.WinDivertHelperEvalFilter
    evaluate.argtypes = [ct.c_char_p, ct.c_void_p, ct.c_uint, ct.c_void_p]
    evaluate.restype = ct.c_int
    def address(layer=1, outbound=False):
        raw = bytearray(80)
        struct.pack_into("<I", raw, 8, layer | (int(outbound) << 17))
        struct.pack_into("<I", raw, 16, index)
        return ct.create_string_buffer(bytes(raw), 80)
    filter_cases = 0
    def matches(name, raw, expected, layer=1, outbound=False, config=active):
        nonlocal filter_cases
        buf = ct.create_string_buffer(raw, len(raw))
        addr = address(layer, outbound)
        result = bool(evaluate(config["filters"][name].encode(), buf, len(raw), addr))
        assert result == expected, (name, raw.hex(), expected)
        filter_cases += 1
    server = "198.51.100.7"
    matches("tcpForward", packet(client, server), True)
    matches("tcpForward", packet(server, client, 22102, 40123), True)
    matches("tcpForward", packet(client, server, dp=443), False)
    matches("tcpForward", packet(client, "198.51.100.8"), False)
    matches("tcpForward", packet(host, server), False)
    matches("tcpForward", packet(client, host), False)
    matches("tcpReturn", packet(host, client, 19001, 20000), True, 0, True)
    matches("tcpReturn", packet(host, client, 19001, 20000), False, 0, False)
    matches("tcpReturn", packet(host, client, 19002, 20000), False, 0, True)
    matches("udp", packet(client, server, protocol=17), True)
    matches("udp", packet(server, client, protocol=17), True)
    matches("udp", packet(host, server, protocol=17), False)
    for port in (80, 443, 1883, 49155, 65010):
        matches("tcpForward", packet(client, server, dp=port), port == 65010, config=fixed)
        matches("tcpForward", packet(server, client, sp=port), port == 65010, config=fixed)
    matches("tcpForward", packet(client, "198.51.100.8", dp=65010), True, config=fixed)
    matches("tcpForward", packet(host, server, dp=65010), False, config=fixed)
    matches("udp", packet(client, server, dp=19404, protocol=17), True, config=fixed)
    matches("udp", packet(server, client, sp=51349, protocol=17), True, config=fixed)
    for port in (1, 80, 443, 19000, 22102, 65535):
        matches("tcpForward", packet(client, server, dp=port), True, config=automatic)
        matches("tcpForward", packet(server, client, sp=port), True, config=automatic)
    peer = str(network.network_address + 3)
    if peer == client or peer == host:
        peer = str(network.broadcast_address - 1)
    for src, dst in ((host, server), (client, host), (client, peer), (peer, client), (server, peer)):
        matches("tcpForward", packet(src, dst), False, config=automatic)
    checksum_cases = 0
    for protocol in (6, 17):
        for src, dst, sp, dp in ((client, host, 20000, 19001), (server, client, 22102, 40123)):
            raw = packet(src, dst, sp, dp, protocol)
            buf = ct.create_string_buffer(raw, len(raw))
            addr = address(0)
            assert calc(buf, len(raw), addr, 0)
            result = buf.raw
            assert checksum(result[:20]) == 0
            pseudo = result[12:20] + bytes([0, protocol]) + struct.pack("!H", len(result) - 20)
            assert checksum(pseudo + result[20:]) == 0
            assert result[-5:] == b"hello"
            checksum_cases += 1
    print(json.dumps({"preflightModes": 4, "invalidConfigurationsRejected": len(invalid),
                      "filterCases": filter_cases, "checksumCases": checksum_cases,
                      "driverOpened": False, "networkSettingsChanged": False}))
    return 0

if __name__ == "__main__":
    sys.exit(main())

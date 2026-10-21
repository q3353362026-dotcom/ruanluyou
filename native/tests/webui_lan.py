"""LAN/loopback HTTP policy checks. No capture, firewall or sharing changes."""
import http.client
import ipaddress
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit


def main():
    exe, assets = map(Path, sys.argv[1:])
    inventory = json.loads(subprocess.check_output([str(exe), 'network-info']))
    networks = [(a['ifIndex'], ip['address']) for a in inventory['adapters'] if a['up']
                for ip in a['ipv4'] if any(ipaddress.ip_address(ip['address']) in ipaddress.ip_network(net)
                                          for net in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16'))]
    lan_connections = 0
    for selection in [None] + networks:
        enabled = selection is not None
        bind_ip = selection[1] if enabled else '127.0.0.1'
        with tempfile.TemporaryDirectory(prefix='radaer-lan-') as folder:
            with socket.socket() as sock:
                sock.bind((bind_ip, 0))
                port = sock.getsockname()[1]
            lan_args = ['--lan-ip', bind_ip, '--lan-if-index', str(selection[0])] if enabled else []
            proc = subprocess.Popen([str(exe), 'webui', '--port', str(port), '--lan', str(enabled).lower(),
                                     '--web-root', str(assets), '--output-directory', folder] + lan_args,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            def request(path, expected=200, host=None, target=bind_ip, body=None, headers=None):
                fields = dict(headers or {})
                if host:
                    fields['Host'] = host
                if body is not None:
                    fields['Content-Type'] = 'application/json'
                conn = http.client.HTTPConnection(target, port, timeout=3)
                try:
                    conn.request('POST' if body is not None else 'GET', path,
                                 json.dumps(body) if body is not None else None, fields)
                    response = conn.getresponse()
                    payload = response.read()
                    assert response.status == expected, (path, host, response.status, payload)
                    return payload
                finally:
                    conn.close()

            try:
                deadline = time.monotonic() + 10
                while True:
                    try:
                        bootstrap = json.loads(request('/api/bootstrap'))
                        break
                    except ConnectionError:
                        if time.monotonic() >= deadline or proc.poll() is not None:
                            raise
                        time.sleep(.05)
                assert bootstrap['lanEnabled'] == enabled
                if enabled:
                    assert bootstrap['lanUrls'] == [f'http://{bind_ip}:{port}']
                    assert bootstrap['lanIfIndex'] == selection[0]
                    for other_ip in {'127.0.0.1'} | {ip for _, ip in networks if ip != bind_ip}:
                        request('/api/live', 403, host=f'{other_ip}:{port}')
                        probe = socket.socket()
                        probe.settimeout(.25)
                        try:
                            assert probe.connect_ex((other_ip, port)) != 0, ('unexpected listener', other_ip)
                        finally:
                            probe.close()
                if not enabled:
                    assert bootstrap['lanUrls'] == []
                    request('/api/bootstrap', 403, host=f'192.168.137.1:{port}')
                request('/api/live', 403, host=f'attacker.test:{port}')
                request('/api/live', 403, headers={'Origin': 'http://attacker.test'})
                request('/api/live', 403, headers={'Sec-Fetch-Site': 'cross-site'})
                request('/api/scan/stop', 403, body={})
                token = bootstrap['token']
                request('/api/scan/stop', body={}, headers={'X-Radaer-Token': token})
                for url in bootstrap['lanUrls']:
                    parts = urlsplit(url)
                    # Exercise the LAN socket as well as its same-origin request policy.
                    result = json.loads(request('/api/bootstrap', host=parts.netloc, target=parts.hostname))
                    assert result['token'] == token
                    assert b'id="overlay"' in request('/radar', host=parts.netloc, target=parts.hostname)
                    request('/api/live', host=parts.netloc, target=parts.hostname)
                    request('/api/scan/stop', host=parts.netloc, target=parts.hostname, body={},
                            headers={'Origin': url, 'X-Radaer-Token': token})
                    request('/api/scan/stop', 403, host=parts.netloc, body={},
                            headers={'Origin': 'http://attacker.test', 'X-Radaer-Token': token})
                    request('/api/scan/stop', 403, host=parts.netloc, body={}, headers={'Origin': url})
                    request('/api/live', 403, host=parts.netloc, headers={'Sec-Fetch-Site': 'cross-site'})
                    lan_connections += 1
                request('/api/server/stop', body={}, headers={'X-Radaer-Token': token})
                _, stderr = proc.communicate(timeout=5)
                assert proc.returncode == 0, stderr.decode(errors='replace')
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    proc.wait(timeout=5)
    # Missing/invalid selection must fail closed, never enable all interfaces.
    for extra in ([], ['--lan-ip', '192.0.2.1', '--lan-if-index', '4294967295']):
        failed = subprocess.run([str(exe), 'webui', '--lan', 'true', '--web-root', str(assets)] + extra,
                                capture_output=True, timeout=5)
        assert failed.returncode != 0
    print(json.dumps({'loopbackAndLanPolicy': True, 'singleInterfaceIsolation': True, 'lanAddressesConnected': lan_connections,
                      'realDriverOpened': False, 'firewallModified': False}))


if __name__ == '__main__':
    main()

"""Root-only integration in disposable network namespaces; never changes host rules."""
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def run(*args, check=True, **kwargs):
    return subprocess.run(list(map(str, args)), check=check, capture_output=True, text=True, **kwargs)


def isolated(exe, assets, client, server):
    with tempfile.TemporaryDirectory(prefix='radaer-linux-') as folder:
        proc = subprocess.Popen([exe, 'webui', '--port', '18765', '--lan', 'true',
                                 '--lan-ip', '192.168.75.1', '--lan-if-index', str(socket.if_nametoindex('lan0')),
                                 '--web-root', assets, '--output-directory', folder],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        token = ''

        def api(path, body=None, expected=200):
            conn = http.client.HTTPConnection('192.168.75.1', 18765, timeout=3)
            try:
                conn.request('GET' if body is None else 'POST', path,
                             None if body is None else json.dumps(body),
                             {'Content-Type': 'application/json', 'X-Radaer-Token': token})
                response = conn.getresponse()
                data = json.loads(response.read())
                assert response.status == expected, (response.status, data)
                return data
            finally:
                conn.close()

        def until(predicate):
            deadline = time.monotonic() + 6
            while True:
                value = predicate()
                if value:
                    return value
                assert proc.poll() is None and time.monotonic() < deadline, 'service/test timed out'
                time.sleep(.05)

        try:
            for _ in range(100):
                try:
                    token = api('/api/bootstrap')['token']
                    break
                except OSError:
                    time.sleep(.05)
            assert token
            run('nft', 'add', 'table', 'ip', 'unrelated_fixture')
            settings = dict(ifIndex=socket.if_nametoindex('lan0'), hostIp='192.168.75.1',
                            clientIp='192.168.75.2', seconds=0, redirectTcp=True, tcpPort=65010,
                            saveLogs=True, device='isolated-test')
            api('/api/capture/preflight', {**settings, 'clientIp': '198.18.0.2'}, 400)
            api('/api/capture/preflight', {**settings, 'tcpPort': 0}, 400)
            api('/api/capture/preflight', {**settings, 'seconds': -1}, 400)
            api('/api/capture/preflight', {**settings, 'seconds': 604801}, 400)
            api('/api/capture/start', settings)
            until(lambda: run('nft', 'list', 'table', 'ip', 'radaer_native', check=False).returncode == 0)
            rules = run('nft', 'list', 'table', 'ip', 'radaer_native').stdout
            assert '192.168.75.2' in rules and '65010' in rules and '192.168.75.0/24' in rules, rules
            # A normal payload must cross the redirect and SO_ORIGINAL_DST relay unchanged.
            echo = subprocess.Popen(['ip', 'netns', 'exec', server, sys.executable, '-c',
                "import socket; s=socket.socket(); s.bind(('198.18.0.2',65010)); s.listen(); "
                "\nfor _ in range(2):\n c,a=s.accept(); c.sendall(c.recv(1024)); c.close()"], stderr=subprocess.PIPE)
            try:
                time.sleep(.1)
                for attempt in range(2):
                    run('ip', 'netns', 'exec', client, sys.executable, '-c',
                        "import socket; s=socket.create_connection(('198.18.0.2',65010),4); "
                        "s.sendall(b'GET /synthetic HTTP/1.0\\r\\n\\r\\n'); "
                        "assert s.recv(1024)==b'GET /synthetic HTTP/1.0\\r\\n\\r\\n'; s.close()", timeout=6)
                    # Closing the target and waiting must leave the capture/listener alive.
                    time.sleep(.3)
                    capture = api('/api/state')['capture']
                    assert capture['running'] and capture['seconds'] == 0, capture
                    assert capture['counts'].get('tcp_connected', 0) == attempt + 1, capture
                assert echo.wait(timeout=3) == 0
            finally:
                if echo.poll() is None:
                    echo.kill()
                echo.communicate()
            udp_echo = subprocess.Popen(['ip', 'netns', 'exec', server, sys.executable, '-c',
                "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); "
                "s.bind(('198.18.0.2',19000)); "
                "\nfor _ in range(2):\n data,addr=s.recvfrom(65535); s.sendto(data,addr)"], stderr=subprocess.PIPE)
            try:
                time.sleep(.1)
                run('ip', 'netns', 'exec', client, sys.executable, '-c',
                    "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(3); "
                    "s.sendto(b'isolated-udp',('198.18.0.2',19000)); "
                    "assert s.recv(1024)==b'isolated-udp'; "
                    "s.sendto(b'f'*4000,('198.18.0.2',19000)); assert s.recv(65535)==b'f'*4000", timeout=5)
                assert udp_echo.wait(timeout=3) == 0
            finally:
                if udp_echo.poll() is None:
                    udp_echo.kill()
                udp_echo.communicate()
            until(lambda: api('/api/state')['capture'].get('counts', {}).get('udp_raw', 0) > 0)
            state = api('/api/state')['capture']
            assert state['counts'].get('tcp_connected', 0) == 2, state
            # Clear all completed app recordings, including more than the listing limit.
            # Never remove the active writer, banks, symlinks, subdirectories or foreign files.
            recording_root = Path(folder)
            old_files = [recording_root / f'web-{n}-abcdef.jsonl' for n in range(201)]
            for path in old_files: path.write_bytes(b'old\n')
            bank_folder = recording_root / 'banks'; bank_folder.mkdir()
            bank_file = bank_folder / ('a' * 64 + '.bin'); bank_file.write_bytes(b'bank')
            foreign = recording_root / 'notes.jsonl'; foreign.write_bytes(b'keep')
            nested = recording_root / 'nested'; nested.mkdir()
            (nested / 'web-1-abcdef.jsonl').write_bytes(b'keep')
            link = recording_root / 'web-999-abcdef.jsonl'; link.symlink_to(foreign)
            assert len(api('/api/replays')) == 200
            valid_token = token; token = 'wrong-token'
            api('/api/logs/clear', {}, 403); token = valid_token
            assert all(path.exists() for path in old_files)
            cleared = api('/api/logs/clear', {})
            assert cleared == dict(removed=201, bytes=804, preservedActive=1, failed=[]), cleared
            assert Path(state['output']).exists() and api('/api/state')['capture']['running']
            assert bank_file.read_bytes() == b'bank' and foreign.read_bytes() == b'keep'
            assert link.is_symlink() and (nested / 'web-1-abcdef.jsonl').exists()
            assert api('/api/logs/clear', {})['removed'] == 0
            def redirected_packets():
                data = json.loads(run('nft', '-j', 'list', 'table', 'ip', 'radaer_native').stdout)
                return sum(expr['counter']['packets'] for item in data['nftables'] if 'rule' in item
                           for expr in item['rule']['expr'] if 'counter' in expr)
            before = redirected_packets()
            assert before > 0
            # Another source, a different port, and router-local traffic must bypass the rule.
            run('ip', '-n', client, 'addr', 'add', '192.168.75.3/24', 'dev', 'cli0')
            # Unrelated LAN UDP must be rejected before it consumes socket space.
            run('ip', 'netns', 'exec', client, sys.executable, '-c',
                "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); "
                "s.bind(('192.168.75.3',0)); "
                "[s.sendto(b'unrelated'*100,('192.168.75.1',19001)) for _ in range(500)]")
            until(lambda: api('/api/state')['capture'].get('stats', {}).get('packetsTotal', 0) >= 8)
            stats = api('/api/state')['capture']['stats']
            assert stats['packetsTotal'] < 50 and stats['dropsTotal'] == 0, stats
            assert stats['kernelFilter'] == 'client_ipv4_udp' and stats['receiveBufferBytes'] > 0, stats
            run('ip', 'netns', 'exec', client, sys.executable, '-c',
                "import socket\n"
                "for source,target,port in [('192.168.75.3','198.18.0.2',65010),"
                "('192.168.75.2','198.18.0.2',65011),('192.168.75.2','192.168.75.1',65010)]:\n"
                " s=socket.socket(); s.settimeout(.3); s.bind((source,0)); s.connect_ex((target,port)); s.close()")
            assert redirected_packets() == before
            api('/api/capture/stop', {})
            until(lambda: not api('/api/state')['capture']['running'])
            capture = api('/api/state')['capture']
            events = [json.loads(line) for line in Path(capture['output']).read_text().splitlines()]
            started = next(e for e in events if e['type'] == 'session_started')
            assert started['data']['savesUdpPayload'] is False
            directions = {e['data']['direction'] for e in events if e['type'] == 'udp_raw'
                          and e['data']['remote'] == '198.18.0.2:19000'}
            assert directions == {'up', 'down'}, ('missing UDP direction', directions)
            assert all('payloadBase64' not in e['data'] for e in events if e['type'] == 'udp_raw'), \
                'default capture must not persist UDP payloads'
            fragmented = [e for e in events if e['type'] == 'udp_raw' and e['data'].get('ipFragmentCount', 1) > 1]
            assert {e['data']['direction'] for e in fragmented} == {'up', 'down'}, 'filter must retain noninitial fragments'
            assert not any('192.168.75.3:' in e['data']['local'] for e in events if e['type'] == 'udp_raw')
            cleared = api('/api/logs/clear', {})
            assert cleared['removed'] == 1 and cleared['preservedActive'] == 0
            assert not Path(capture['output']).exists() and api('/api/state')['capture']['output'] == ''
            assert run('nft', 'list', 'table', 'ip', 'radaer_native', check=False).returncode != 0
            run('nft', 'list', 'table', 'ip', 'unrelated_fixture')
            # UDP-only capture must not install NAT, even if a stale UI supplied a TCP port.
            api('/api/capture/start', {**{k: v for k, v in settings.items() if k != 'seconds'}, 'redirectTcp': False})
            until(lambda: api('/api/state')['capture']['phase'] == 'running')
            assert api('/api/state')['capture']['seconds'] == 0
            assert run('nft', 'list', 'table', 'ip', 'radaer_native', check=False).returncode != 0
            api('/api/capture/stop', {})
            until(lambda: not api('/api/state')['capture']['running'])
            assert api('/api/state')['capture']['phase'] == 'completed'
            # Finite capture still expires automatically.
            api('/api/capture/start', {**settings, 'seconds': 1, 'redirectTcp': False})
            until(lambda: not api('/api/state')['capture']['running'])
            completed = api('/api/state')['capture']
            assert completed['phase'] == 'completed'
            assert any(e['type'] == 'session_stopped' and e['data']['reason'] == 'duration_elapsed'
                       for e in completed['events'])
            # SIGTERM must remove a live redirect as well.
            api('/api/capture/start', settings)
            until(lambda: run('nft', 'list', 'table', 'ip', 'radaer_native', check=False).returncode == 0)
            proc.terminate()
            _, errors = proc.communicate(timeout=6)
            assert proc.returncode == 0, errors
            assert run('nft', 'list', 'table', 'ip', 'radaer_native', check=False).returncode != 0
            run('nft', 'list', 'table', 'ip', 'unrelated_fixture')
            print('Linux capture: reconnect, unlimited/finite duration, bidirectional UDP, NAT and stop/SIGTERM cleanup passed')
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.communicate(timeout=6)


def main():
    if len(sys.argv) > 3 and sys.argv[3] == '--isolated':
        return isolated(sys.argv[1], sys.argv[2], sys.argv[4], sys.argv[5])
    if os.geteuid() != 0 or not shutil.which('ip') or not shutil.which('nft'):
        print('SKIP: Linux capture integration requires root, iproute2 and nftables')
        return 77
    names = [f'radaer-{os.getpid()}-{suffix}' for suffix in ('router', 'client', 'server')]
    router, client, server = names
    created = []
    try:
        for name in names:
            run('ip', 'netns', 'add', name)
            created.append(name)
            run('ip', '-n', name, 'link', 'set', 'lo', 'up')
        for dev, peer, address, other, peer_ip in (
                ('lan0', 'cli0', '192.168.75.1/24', client, '192.168.75.2/24'),
                ('wan0', 'srv0', '198.18.0.1/24', server, '198.18.0.2/24')):
            run('ip', '-n', router, 'link', 'add', dev, 'type', 'veth', 'peer', 'name', peer)
            run('ip', '-n', router, 'link', 'set', peer, 'netns', other)
            for ns, device, ip in ((router, dev, address), (other, peer, peer_ip)):
                run('ip', '-n', ns, 'addr', 'add', ip, 'dev', device)
                run('ip', '-n', ns, 'link', 'set', device, 'up')
        run('ip', '-n', client, 'route', 'add', 'default', 'via', '192.168.75.1')
        run('ip', '-n', server, 'route', 'add', 'default', 'via', '198.18.0.1')
        run('ip', 'netns', 'exec', router, 'sysctl', '-qw', 'net.ipv4.ip_forward=1')
        result = run('ip', 'netns', 'exec', router, sys.executable, Path(__file__).resolve(),
                     Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), '--isolated', client, server,
                     check=False)
        print(result.stdout, end='')
        print(result.stderr, end='', file=sys.stderr)
        return result.returncode
    finally:
        for name in reversed(created):
            run('ip', 'netns', 'del', name, check=False)


if __name__ == '__main__':
    sys.exit(main())

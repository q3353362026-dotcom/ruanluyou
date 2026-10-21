"""Local HTTP/API checks, passive neighbor discovery and synthetic player capture; no real driver."""
import base64
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from integration import crypt_udp
from player_fixtures import W, player_records, event, packet, bunch, movement_payload, self_event, actor_prefix
from health_integration import exports, sub, health_body
from loot_integration import pickup_open
from equipment_integration import gear, held, extra_field, sub as equipment_sub

def main():
    exe, fixture, assets = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="radaer-webui-") as folder:
        root = Path(folder)
        replay_dir=root/'sessions';replay_dir.mkdir()
        replay_id='web-1000-abcdef.jsonl'
        replay_row=dict(flow='client:1>server:2',epoch=1,channel=14,slot=1,track='p',actorGuid=4,x=100,y=200,z=0,
                        timestampMs=1100,status='plausible',name='回放玩家',teamId=7,equipment={'armor':{'level':3}})
        def recorded(kind,time,data):return dict(type=kind,timestampMs=time,session='replay',data=data)
        replay_events=[recorded('session_started',1000,{}),
            recorded('udp_plaintext',1001,dict(local='client:1',remote='server:2',epoch=1,timestampMs=1001,direction='down',structuralValid=True,payloadBase64='NOT_FOR_REPLAY')),
            recorded('battle_welcome',1002,dict(flow='client:1>server:2',epoch=1,map='/Game/TestMap')),
            recorded('player_validation',1100,replay_row),
            recorded('self_validation',1101,{**replay_row,'actorGuid':8,'x':50}),
            recorded('player_state',1200,dict(flow='client:1>server:2',epoch=1,actorGuid=4,equipment={'timestampMs':1200,'armor':{'level':4}},vitals={'life':{'timestampMs':1200,'state':'downed'}})),
            recorded('session_stopped',21000,{})]
        (replay_dir/replay_id).write_text(''.join(json.dumps(e)+'\n' for e in replay_events),encoding='utf-8')
        dll = root / "WinDivert.dll"
        shutil.copyfile(fixture, dll)
        (root / "WinDivert64.sys").write_bytes(b"TEST ONLY")
        packets = root / "empty-packets.bin"
        packets.write_bytes(b"")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]
        env = dict(os.environ, RADAER_MOCK_PACKETS=str(packets))
        proc = subprocess.Popen([str(exe), "webui", "--port", str(port), "--web-root", str(assets),
                                 "--output-directory", str(root / "sessions"), "--windivert-dll", str(dll)],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        token = ""
        checks = 0
        def request(path, body=None, expected=200, headers=None, authenticated=True):
            nonlocal checks
            h = dict(headers or {})
            if body is not None:
                h.setdefault("Content-Type", "application/json")
                if authenticated: h.setdefault("X-Radaer-Token", token)
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
            connection.request("POST" if body is not None else "GET", path,
                               json.dumps(body) if body is not None else None, h)
            response = connection.getresponse(); payload = response.read(); response_headers = dict(response.getheaders())
            connection.close(); checks += 1
            assert response.status == expected, (path, response.status, payload)
            return payload, response_headers
        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    payload, _ = request("/api/bootstrap"); bootstrap = json.loads(payload); token = bootstrap["token"]; break
                except ConnectionError:
                    if time.monotonic() > deadline or proc.poll() is not None: raise
                    time.sleep(.1)
            html, headers = request("/")
            assert "局域网设备" in html.decode() and "frame-ancestors 'none'" in headers["Content-Security-Policy"]
            assert 'id="tcpPort"' not in html.decode() and 'id="listenPort"' not in html.decode()
            assert 'id="tcpServer"' not in html.decode() and 'TCP 接入（端口 65010）' in html.decode()
            assert 'id="redirectTcp" type="checkbox" checked' in html.decode()
            assert 'id="tcpFields" class="hidden"' not in html.decode()
            assert 'id="maxLogBytes" type="number"' in html.decode()
            assert 'id="battleStatus"' in html.decode() and 'id="battleDetail"' in html.decode()
            for element in ("decryptRate", "coordinateRate", "coordinateSamplesRate", "coordinateFreshness", "reassemblyStatus"):
                assert f'id="{element}"' in html.decode()
            assert 'href="/radar"' in html.decode() and 'id="radarSvg"' not in html.decode()
            radar,_=request('/radar'); assert 'id="overlay"' in radar.decode() and 'id="roster"' in radar.decode()
            request('/radar/'); request('/radar/model.js'); request('/radar/view.js'); request('/radar/view.css')
            request('/radar/replay.js')
            recordings,_=request('/api/replays');assert json.loads(recordings)[0]['id']==replay_id
            request('/api/replay/load',{'id':'../secret.jsonl'},400)
            request('/api/replay/load',{'id':replay_id},403,authenticated=False)
            playback,_=request('/api/replay/load',{'id':replay_id});playback=json.loads(playback)
            assert playback['format']=='radaer-replay-v1' and playback['endMs']==21000
            assert 'NOT_FOR_REPLAY' not in json.dumps(playback),'raw plaintext must not be exposed'
            assert playback['rowPatches'] is True
            player_deltas=[f['players']['14:1']['set'] for f in playback['frames'] if f.get('players',{}).get('14:1')]
            assert player_deltas[0]['equipment']['armor']['level']==3
            assert player_deltas[-1]['equipment']['armor']['level']==4 and player_deltas[-1]['vitals']['life']['state']=='downed'
            assert any(f.get('self',{}).get('kind')=='self' for f in playback['frames'] if f.get('self'))
            assert any(f.get('battle',{}).get('status')=='idle' for f in playback['frames'])
            assert playback['frames'][-1]['battle']['status']=='stopped'
            request('/radar/maps.json'); request('/vendor/leaflet/leaflet.js'); request('/vendor/leaflet/leaflet.css')
            catalog,_=request('/radar/items.json');catalog=json.loads(catalog)
            assert catalog['count']==3456 and catalog['items']['15080050142']['grade']==6
            assert 'id="lootPanel"' in radar.decode() and 'id="toggleLoot"' in radar.decode()
            operators,_=request('/radar/operators.json');operators=json.loads(operators)['operators']
            assert operators['88000000027']['name']=='蜂医' and operators['88000000036']['name']=='佐娅'
            for operator in operators.values():
                if not operator['portrait']:continue
                png,png_headers=request(operator['portrait'])
                assert png_headers['Content-Type']=='image/png' and png.startswith(b'\x89PNG\r\n\x1a\n')
                assert png==(assets/operator['portrait'].lstrip('/')).read_bytes(),'portrait bytes unchanged'
            request('/radar/portraits/unknown.png',expected=404)
            request('/radar/portraits/../operators.json',expected=404)
            request('/radar/../capture.jsonl',expected=404)
            request("/app.js"); request("/styles.css"); request("/radar.js")
            live,_=request('/api/live');live=json.loads(live)
            assert 'scan' not in live and 'events' not in live and 'configuration' not in live
            assert live['players']==[] and live['self'] is None and live['rates']['selfSamplesPerSecond']==0,live
            request('/api/live',expected=403,headers={'Sec-Fetch-Site':'cross-site'})
            request("/api/scan/start", {}, 403, authenticated=False)
            request("/api/scan/start", {}, 403, headers={"Origin":"https://example.test"})
            request("/api/bootstrap", expected=403, headers={"Host":"attacker.test"})
            request("/api/bootstrap", expected=403, headers={"Sec-Fetch-Site":"cross-site"})
            request("/api/scan/start", {"hostIp":"198.51.100.1","ifIndex":4294967295}, 400)
            request("/api/capture/start", {}, 400)
            request("/api/capture/preflight", {"seconds":-1}, 400)
            request("/api/capture/preflight", {"bankIds":["../../secret"]}, 400)
            request("/api/capture/preflight", {"maxLogBytes": 1023}, 400)
            request("/api/capture/preflight", {"maxLogBytes": 2147483649}, 400)
            request("/api/capture/preflight", {"x":"x"*17000}, 413)
            request("/api/capture/report", {}, 400)
            request("/api/banks")
            chosen = None
            for adapter in bootstrap["network"]["adapters"]:
                for address in adapter["ipv4"]:
                    host = ipaddress.ip_address(address["address"])
                    net = ipaddress.ip_network(f"{host}/{address['prefixLength']}", strict=False)
                    if adapter["up"] and not host.is_loopback and not host.is_link_local and net.num_addresses >= 8:
                        candidate = net.network_address + 2
                        if candidate == host: candidate += 1
                        chosen = {"ifIndex":adapter["ifIndex"],"hostIp":str(host),"clientIp":str(candidate)}; break
                if chosen: break
            lifecycle = False
            if chosen:
                request("/api/scan/start", {**chosen,"active":False})
                for _ in range(40):
                    payload, _ = request("/api/state"); state = json.loads(payload)
                    if not state["scan"]["running"]: break
                    time.sleep(.05)
                assert not state["scan"]["running"] and not state["scan"]["error"]
                assert any(d["self"] and d["ip"] == chosen["hostIp"] for d in state["scan"]["devices"])
                payload, _ = request("/api/capture/preflight", {**chosen,"seconds":30,"device":"webui-test"})
                default_tcp = json.loads(payload)
                assert default_tcp['tcpMode']=='filtered' and 'tcp.DstPort == 65010' in default_tcp['filters']['tcpForward']
                payload, _ = request("/api/capture/preflight", {**chosen,"redirectTcp":False})
                passive = json.loads(payload)
                assert passive['tcpMode']=='disabled' and not passive['firewallManaged']
                payload, _ = request("/api/capture/preflight", {**chosen,"redirectTcp":True})
                automatic = json.loads(payload)
                assert automatic["tcpMode"] == "filtered" and automatic["returnFilterPendingBind"]
                assert "tcp.DstPort == 65010" in automatic["filters"]["tcpForward"]
                assert "tcp.SrcPort == 65010" in automatic["filters"]["tcpForward"]
                assert automatic["filters"]["tcpReturn"] is None and automatic["firewallManaged"]
                if bootstrap["network"]["elevated"]:
                    bank=bytes((i*37+29)&255 for i in range(128))
                    bank_id=hashlib.sha256(bank).hexdigest()
                    bank_dir=root/"sessions/banks";bank_dir.mkdir(parents=True)
                    (bank_dir/(bank_id+".bin")).write_bytes(bank)
                    wire=[]
                    test_records=player_records()+[event(packet([bunch(movement_payload(mask=17),seq=20)],23),23)]
                    equipment=exports([(17,4,'CharacterEquip'),(19,4,'WeaponReplicate')]).join(equipment_sub(gear(),17)).join(equipment_sub(extra_field(held()),19))
                    test_records[3]=event(packet([bunch(actor_prefix(2,3,'MovementReplicationActor'),opened=True,exports=True),bunch(equipment,channel=3,seq=2,exports=True)],4),4)
                    test_records += [self_event(i,ts=100+i*.01,full=i%2==0) for i in range(30,35)]
                    health_start=exports([(11,0,'BP_PropertyReplicationCharacterHealth_C'),(13,10,'PropertyReplication'),(15,4,'AttributeCenter')]).packed(10).packed(11).packed(0).bits(0,4)
                    test_records.append(event(packet([bunch(health_start.join(sub(health_body({1:0,6:3,7:15}))),channel=17,opened=True,exports=True)],35),35))
                    # Switch weapon, then update a remote pose after the downed state event.
                    switched=equipment_sub(extra_field(held(456),456,18020000001),19)
                    test_records.append(event(packet([pickup_open(),bunch(switched,channel=3,seq=3),bunch(movement_payload(slots=(17,),offset=17),seq=21)],36),36))
                    for i,record in enumerate(test_records):
                        up=record['data']['direction']=='up'
                        raw=base64.b64decode(record["data"]["payloadBase64"])
                        payload=b'\0'*8+raw if up else crypt_udp(raw,bank)
                        ports=(40123,22102) if up else (22102,40123)
                        udp=struct.pack("!HHHH",*ports,8+len(payload),0)+payload
                        src,dst=(chosen['clientIp'],'198.51.100.7') if up else ('198.51.100.7',chosen['clientIp'])
                        ip=struct.pack("!BBHHHBBH4s4s",0x45,0,20+len(udp),i,0,64,17,0,
                            socket.inet_aton(src),socket.inet_aton(dst))
                        cut=max(8,len(udp)//2//8*8)
                        for offset,part,more in [(cut,udp[cut:],False),(0,udp[:cut],True)]:
                            header=bytearray(ip)
                            struct.pack_into('!H',header,2,20+len(part))
                            struct.pack_into('!H',header,6,(0x2000 if more else 0)|offset//8)
                            wire.append(bytes(header)+part)
                    packets.write_bytes(b"".join(struct.pack("<I",len(p))+p for p in wire))
                    request("/api/capture/start", {**chosen,"seconds":30,"device":"webui-test","bankIds":[bank_id],"redirectTcp":False})
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        payload, _ = request("/api/state"); state = json.loads(payload)
                        if state["capture"]["counts"].get("player_sample")==38 and state['capture']['counts'].get('self_sample')==5 and state['capture']['counts'].get('player_state',0)>=2 and state['capture']['counts'].get('loot_state',0)>=2: break
                        assert state["capture"]["running"], state["capture"]
                        time.sleep(.05)
                    assert state["capture"]["maxLogBytes"] == 67108864,state["capture"]
                    assert state["capture"]["configuration"]["maxLogBytes"] == 67108864,state["capture"]
                    assert state["capture"]["counts"].get("shared_capture_started") == 1
                    request('/api/replay/load',{'id':Path(state['capture']['output']).name},400)
                    assert state["capture"]["counts"].get("player_sample") == 38,state["capture"]
                    assert state['capture']['counts'].get('self_sample')==5,state['capture']
                    live,_=request('/api/live');live=json.loads(live)
                    assert live['battle']['status']=='live' and live['self']['teamId']==7,live
                    assert live['loot']['100']['itemId']=='15080050142' and live['loot']['100']['x']==5,live['loot']
                    assert live['rates']['selfSamplesPerSecond']==5 and len(live['players'])==2,live
                    assert live['revision']>=state['capture']['revision']
                    assert all('track' in p and 'poseSequence' in p for p in live['players'])
                    own=state['capture']['self']
                    assert own['teamId']==7 and own['actorGuid']==4 and own['x']==10000.1 and own['yaw']==90,own
                    assert state["capture"]["counts"].get("player_decode_warning",0) == 0,state["capture"]
                    assert not any(e["type"]=="player_decode_warning" for e in state["capture"]["events"]),state["capture"]
                    assert len(state["capture"]["players"]) == 2,state["capture"]
                    assert state["capture"]["battle"]["status"] == "live",state["capture"]
                    assert state["capture"]["battle"]["map"] == "/Game/TestMap",state["capture"]
                    named=next(p for p in state["capture"]["players"] if p["slot"]==17)
                    for shown in (named,next(p for p in live['players'] if p['slot']==17)):
                        assert shown['equipment']['helmet']['level']==2 and shown['equipment']['armor']['level']==1,shown
                        assert shown['equipment']['weapon']['identity']==456,shown
                    assert named['vitals']['health']['Health']['value']==0 and named['vitals']['life']['state']=='downed',named
                    assert live['self']['vitals']['life']['state']=='downed' and live['self']['vitals']['health']['Health']['value']==0,live
                    assert named["name"]=="测试玩家" and named["teamId"]==7 and named["x"]==159.4,named
                    rates=state["capture"]["rates"]
                    assert rates["windowMs"]==1000 and rates["udpDecryptPacketsPerSecond"]==25,rates
                    assert rates["coordinatePacketsPerSecond"]==20 and rates["coordinateSamplesPerSecond"]==38,rates
                    prefix=next(p for p in state["capture"]["players"] if p["slot"]==0)
                    assert prefix["decodeCompleteness"]=="known_prefix" and not prefix["movementFrameComplete"],prefix
                    # Mock packets can share a millisecond; timestamp rejection is still valid.
                    assert "partial_movement_block" in prefix["flags"] and "decoder_error_during_sample" not in prefix["flags"],prefix
                    assert prefix["updatesPerSecond"]==19 and named["decodeCompleteness"]=="complete"
                    assert named["updatesPerSecond"]==19 and named["lastUpdateAgeMs"] is not None,named
                    assert named["sourceIntervalMs"] is not None,named
                    assert not any(e["type"] in ("player_sample","player_validation","self_sample","self_validation") for e in state["capture"]["events"])
                    request("/api/capture/start", chosen, 400)
                    # No incoming packets: the capture timer, not new traffic or browser logic, must expire the flow.
                    deadline = time.monotonic() + 18
                    while time.monotonic() < deadline:
                        payload, _ = request("/api/state"); state = json.loads(payload)
                        if state["capture"]["battle"]["status"] == "idle": break
                        time.sleep(.25)
                    assert state["capture"]["battle"]["status"] == "idle",state["capture"]
                    assert state["capture"]["players"] == [] and state["capture"]["running"]
                    assert state['capture']['self'] is None
                    live,_=request('/api/live');live=json.loads(live)
                    assert live['self'] is None and live['players']==[] and live['rates']['selfSamplesPerSecond']==0,live
                    assert live['loot']=={},live
                    assert all(state["capture"]["rates"][key]==0 for key in
                               ("udpDecryptPacketsPerSecond","coordinatePacketsPerSecond","coordinateSamplesPerSecond"))
                    request("/api/capture/stop", {})
                    for _ in range(60):
                        payload, _ = request("/api/state"); state = json.loads(payload)
                        if not state["capture"]["running"]: break
                        time.sleep(.05)
                    assert state["capture"]["phase"] == "completed", state["capture"]
                    assert state["capture"]["battle"]["status"] == "stopped" and not state["capture"]["players"]
                    assert state["capture"]["rates"]["udpDecryptPacketsPerSecond"]==0
                    assert state['capture']['reassembly']['ipv4DatagramsReassembled']==len(test_records)
                    assert state['capture']['reassembly']['ipv4FragmentsReceived']==len(wire)
                    assert state['capture']['reassembly']['ipv4ReassemblyTimeouts']==0
                    events = [json.loads(line) for line in Path(state["capture"]["output"]).read_text(encoding="utf-8").splitlines()]
                    assert events[-1]["type"] == "session_stopped" and events[-1]["data"]["reason"] == "requested_stop"
                    assert sum(e['type']=='self_sample' for e in events)==5
                    assert sum(e['type']=='self_validation' for e in events)==5
                    assert all(e['data']['ipFragmentCount']==2 for e in events if e['type']=='udp_plaintext')
                    states=[e["data"]["status"] for e in events if e["type"]=="battle_state"]
                    assert states==["waiting","live","idle","stopped"],states
                    request("/api/capture/report", {}); lifecycle = True
                    before={str(p):p.stat().st_size for p in replay_dir.rglob('*') if p.is_file()}
                    request('/api/capture/start',{**chosen,'seconds':30,'device':'no-logs-test',
                            'bankIds':[bank_id],'redirectTcp':False,'saveLogs':False,'saveBanks':True})
                    for _ in range(100):
                        payload,_=request('/api/state'); current=json.loads(payload)['capture']
                        if current['counts'].get('player_sample',0)>=38: break
                        time.sleep(.02)
                    assert current['counts'].get('player_sample')==38,current
                    assert current['output']=='' and current['saveLogs'] is False,current
                    live,_=request('/api/live');live=json.loads(live)
                    assert len(live['players'])==2 and live['self'] is not None,live
                    assert current['events'],current
                    request('/api/capture/stop',{})
                    for _ in range(100):
                        payload,_=request('/api/state')
                        if not json.loads(payload)['capture']['running']: break
                        time.sleep(.02)
                    request('/api/capture/report',{},400)
                    after={str(p):p.stat().st_size for p in replay_dir.rglob('*') if p.is_file()}
                    assert after==before,(before,after)

            request("/api/server/stop", {})
            stdout, stderr = proc.communicate(timeout=10)
            assert proc.returncode == 0, stderr.decode(errors="replace")
            print(json.dumps({"httpChecks":checks,"neighborOnlyDiscovery":bool(chosen),
                              "mockCaptureStartStop":lifecycle,"nativePlayerSamples":38 if lifecycle else 0,
                              "realDriverOpened":False}))
        finally:
            if proc.poll() is None: proc.terminate(); proc.wait(timeout=10)
    return 0

if __name__ == "__main__":
    sys.exit(main())

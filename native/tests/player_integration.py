"""Native player CLI/lifecycle tests using synthetic UE packets; no reference runtime."""
import base64
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from player_fixtures import W, actor_prefix, bunch, content, event, frame, movement_payload, packet, player_records
from integration import crypt_udp


def run(exe, *args):
    p = subprocess.run([str(exe), *map(str, args)], capture_output=True, encoding="utf-8", timeout=30)
    assert p.returncode == 0, (p.stdout, p.stderr)
    return p.stdout


def lines(path):
    return [json.loads(s) for s in path.read_text(encoding="utf-8").splitlines()]


def write(path, records):
    path.write_text("".join(json.dumps(r) + "\n" for r in records), encoding="utf-8")


def main():
    exe = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="radaer-player-cli-") as temp:
        root = Path(temp)
        source = root / "plain.jsonl"
        records = player_records()
        write(source, records)
        summary = json.loads(run(exe, "decode-players", "--in", source, "--output-directory", root / "decoded"))
        samples = lines(root / "decoded/players.jsonl")
        assert summary["errors"] == summary["exceptions"] == 0, summary
        assert len(samples) == 36 and all(s["data"]["nativeUEPort"] for s in samples)
        named = [s["data"] for s in samples if s["data"]["slot"] == 17]
        assert all(s["name"] == "测试玩家" and s["teamId"] == 7 for s in named), named
        assert [s["x"] for s in named] == [142.4 + i for i in range(18)]
        quality = lines(root / "decoded/player-quality.jsonl")
        assert sum(e["data"]["status"] == "plausible" for e in quality) == 32, quality
        assert not any(e["data"]["cryptographicAuthenticated"] for e in quality)
        battle_events=lines(root/"decoded/battle-events.jsonl")
        assert len([e for e in battle_events if e["type"]=="battle_welcome"])==1,battle_events
        active=[e["data"] for e in battle_events if e["type"]=="battle_state" and e["data"]["status"]=="live"]
        assert len(active)==1 and active[0]["map"]=="/Game/TestMap",active
        # A second flow with valid movement must wait for its own Welcome, then switch without mixing.
        b=copy.deepcopy(records)
        for e in b:
            e["data"]["remote"]="203.0.113.8:3333"
            e["timestampMs"]+=1000;e["data"]["timestampMs"]=e["timestampMs"]
        delayed_welcome=copy.deepcopy(b[0]);delayed_welcome["timestampMs"]+=1000
        delayed_welcome["data"]["timestampMs"]=delayed_welcome["timestampMs"]
        late_raw={"type":"udp_raw","timestampMs":delayed_welcome["timestampMs"]+15001,"data":{}}
        resumed=copy.deepcopy(b[-1]);resumed["timestampMs"]=late_raw["timestampMs"]+10
        resumed["data"]["timestampMs"]=resumed["timestampMs"]
        write(root/"battle.jsonl",records+b[1:]+[delayed_welcome,late_raw,resumed])
        run(exe,"decode-players","--in",root/"battle.jsonl","--output-directory",root/"battle")
        transitions=[e["data"] for e in lines(root/"battle/battle-events.jsonl") if e["type"]=="battle_state"]
        assert [s["status"] for s in transitions]==["waiting","live","live","idle","live"],transitions
        assert transitions[2]["flow"].endswith("203.0.113.8:3333") and len(transitions[2]["players"])==2,transitions
        assert all(p["flow"]==transitions[2]["flow"] for p in transitions[2]["players"])
        assert transitions[3]["players"]==[] and transitions[4]["reason"]=="udp_resumed"
        # Welcome fallback in the original scanner is bounded to the first 128 bytes, and requires NMT_WELCOME.
        for label,body,expected in (
            ("welcome-fallback",W().bits(1,8).bits(0xffffffff,32).utf8("/Game/Fallback"),True),
            ("not-welcome",W().bits(2,8).string("/Game/Fake"),False),
            ("invalid-welcome",W().bits(1,8).string("/Other/Map"),False)):
            write(root/(label+".jsonl"),[event(packet([bunch(body,channel=0)],1),1)])
            run(exe,"decode-players","--in",root/(label+".jsonl"),"--output-directory",root/label)
            found=[e for e in lines(root/label/"battle-events.jsonl") if e["type"]=="battle_welcome"]
            assert bool(found)==expected,(label,found)
        # Closing and reopening actor channels preserves UE reliable sequencing.
        closed = event(packet([bunch(W(), channel=3, seq=2, closed=True),
                               bunch(W(), channel=4, seq=2, closed=True)], 23), 23)
        after_close = event(packet([bunch(movement_payload(), seq=20)], 24), 24)
        ps = W().bit(0).packed(27).string("新玩家").packed(62).bits(9,32).packed(0)
        char = W().bit(0).packed(18).packed(10).packed(60).packed(17).packed(0)
        reopen = event(packet([
            bunch(actor_prefix(10,7,"BP_SOLPlayerState_C").join(content(ps)),channel=4,seq=3,opened=True,exports=True),
            bunch(actor_prefix(8,5,"BP_DFMCharacter_C",True,True).join(content(char)),channel=3,seq=3,opened=True,exports=True),
            bunch(movement_payload(),seq=21)], 25), 25)
        gap = {"type":"capture_gap","session":"ue-test","data":{"reason":"test"}}
        unbound = event(packet([bunch(movement_payload(),seq=22)],26),26)
        write(root/"lifecycle.jsonl",records+[closed,after_close,reopen,gap,unbound])
        run(exe,"decode-players","--in",root/"lifecycle.jsonl","--output-directory",root/"lifecycle")
        tail=[s["data"] for s in lines(root/"lifecycle/players.jsonl") if s["data"]["slot"]==17 and s["data"]["timestampMs"]>=1700000000230]
        assert [s["name"] for s in tail] == ["新玩家",None],tail
        assert tail[0]["teamId"]==9 and tail[0]["actorGuid"]==8
        assert len({s["track"] for s in tail})==2
        # Closed characters cannot be resurrected as anonymous slots by later deltas.
        assert all(s["timestampMs"]!=1700000000240 for s in tail)
        assert not tail[-1]["handshakeOk"]
        epoch=event(packet([bunch(movement_payload(),seq=22)],26),26,epoch=2)
        write(root/"epoch.jsonl",records+[epoch])
        run(exe,"decode-players","--in",root/"epoch.jsonl","--output-directory",root/"epoch")
        assert all(s["data"]["name"] is None for s in lines(root/"epoch/players.jsonl")[-2:])
        write(root/"retired-epoch.jsonl",records+[epoch,records[-1]])
        retired=json.loads(run(exe,"decode-players","--in",root/"retired-epoch.jsonl","--output-directory",root/"retired-epoch"))
        assert retired["samples"]==38,retired
        # Malformed movement emits no coordinates; the next complete block resets continuity.
        truncated=frame();truncated.b=truncated.b[:-65]
        bad=event(packet([bunch(content(W().bit(0).packed(21).join(truncated)),seq=20)],23),23)
        good=event(packet([bunch(movement_payload(),seq=21)],24),24)
        write(root/"error.jsonl",records+[bad,good])
        run(exe,"decode-players","--in",root/"error.jsonl","--output-directory",root/"error")
        q=lines(root/"error/player-quality.jsonl")
        assert len(q)==38 and all(s["data"]["status"]=="warming_up" for s in q[-2:]),q[-2:]
        assert all(s["data"]["flags"]==["decoder_continuity_reset"] and
                   s["data"]["consecutivePlausibleSamples"]==1 for s in q[-2:]),q[-2:]
        assert all(s["data"]["sourcePacketOrdinal"]!=23 for s in q),q[-2:]
        # Even in the same packet, a later bounded block survives a preceding malformed block.
        mixed=event(packet([bunch(content(W().bit(0).packed(21).join(truncated)),seq=20),
                            bunch(movement_payload(),seq=21)],23),23)
        write(root/"mixed-error.jsonl",records+[mixed])
        run(exe,"decode-players","--in",root/"mixed-error.jsonl","--output-directory",root/"mixed-error")
        mixed_q=lines(root/"mixed-error/player-quality.jsonl")
        assert len(mixed_q)==38 and all(s["data"]["status"]=="warming_up" for s in mixed_q[-2:]),mixed_q[-2:]
        # A known prefix alone cannot establish recovery after a decoder error.
        partial_after_error=event(packet([bunch(movement_payload(mask=17),seq=21)],24),24)
        recovery_full=event(packet([bunch(movement_payload(),seq=22)],25),25)
        write(root/"prefix-error.jsonl",records+[bad,partial_after_error,recovery_full])
        run(exe,"decode-players","--in",root/"prefix-error.jsonl","--output-directory",root/"prefix-error")
        prefix_q=lines(root/"prefix-error/player-quality.jsonl")
        assert len(prefix_q)==39 and prefix_q[-3]["data"]["status"]=="rejected",prefix_q[-3:]
        assert "decoder_error_during_sample" in prefix_q[-3]["data"]["flags"],prefix_q[-3]
        assert all(s["data"]["status"]=="warming_up" for s in prefix_q[-2:]),prefix_q[-2:]
        # Known prefixes retain coordinates without fabricating slots beyond the unknown object.
        for label,payload,count,frame_complete in (
            ("native-prefix",movement_payload(mask=17),1,False),
            ("native-only",movement_payload(mask=16),0,False),
            ("property-prefix",content(W().bit(0).packed(21).join(frame()).packed(22).bits(0xabcdef,24)),2,True)):
            partial=event(packet([bunch(payload,seq=20)],23),23)
            write(root/(label+".jsonl"),records+[partial,good])
            res=json.loads(run(exe,"decode-players","--in",root/(label+".jsonl"),"--output-directory",root/label))
            q=lines(root/label/"player-quality.jsonl")
            assert res["errors"]==0 and res["warnings"]==int(frame_complete) and len(q)==38+count,(label,res)
            warnings=lines(root/label/"decoder-warnings.jsonl")
            assert not any("mask 0x10" in e["data"]["error"] for e in warnings),(label,warnings)
            assert all(s["data"]["status"]=="plausible" for s in q[36:]),(label,q[36:])
            for s in q[36:36+count]:
                d=s["data"]
                assert d["decodeCompleteness"]=="known_prefix" and "partial_movement_block" in d["flags"],d
                assert d["movementFrameComplete"]==frame_complete and not d["movementBlockComplete"],d
            assert all(s["data"]["decodeCompleteness"]=="complete" for s in q[-2:])
        # Partial frames cannot identify a new channel; a full valid frame can establish it.
        unknown=event(packet([bunch(movement_payload(mask=17),seq=1,channel=90)],23),23)
        write(root/"unconfirmed.jsonl",[records[0],unknown])
        res=json.loads(run(exe,"decode-players","--in",root/"unconfirmed.jsonl","--output-directory",root/"unconfirmed"))
        assert res["samples"]==0 and res["errors"]==1 and res["warnings"]==0,res
        confirmed=event(packet([bunch(movement_payload(),channel=90,seq=1)],22),22)
        unknown=event(packet([bunch(movement_payload(mask=17),seq=2,channel=90)],23),23)
        write(root/"confirmed.jsonl",[records[0],confirmed,unknown])
        res=json.loads(run(exe,"decode-players","--in",root/"confirmed.jsonl","--output-directory",root/"confirmed"))
        assert res["samples"]==3 and res["errors"]==0 and res["warnings"]==0,res
        # An unrelated channel error must not poison movement or discard its reliable fragments.
        whole=movement_payload();a=W();a.b=whole.b[:91];b=W();b.b=whole.b[91:]
        start=event(packet([bunch(a,seq=20,partial=(True,False))],23),23)
        other=event(packet([bunch(content(W().bit(0).packed(9999)),channel=4,reliable=False),
                            bunch(movement_payload(),channel=91,reliable=False)],24),24)
        finish=event(packet([bunch(b,seq=21,partial=(False,True))],25),25)
        write(root/"channel-isolation.jsonl",records+[start,other,finish])
        res=json.loads(run(exe,"decode-players","--in",root/"channel-isolation.jsonl","--output-directory",root/"channel-isolation"))
        q=lines(root/"channel-isolation/player-quality.jsonl")
        assert res["samples"]==40 and res["errors"]==1,res
        assert all(s["data"]["status"]=="plausible" for s in q[-2:]),q[-2:]
        errors=lines(root/"channel-isolation/decoder-errors.jsonl")
        assert errors[0]["data"]["channel"]==4 and errors[0]["data"]["scope"]=="channel",errors
        # A thrown error in an unreliable bunch must also preserve other channels and later bunches.
        other=event(packet([bunch(content(W().bit(0).packed(21).join(truncated)),channel=92,reliable=False),
                            bunch(movement_payload(),channel=91,reliable=False)],24),24)
        write(root/"unreliable-isolation.jsonl",records+[start,other,finish])
        res=json.loads(run(exe,"decode-players","--in",root/"unreliable-isolation.jsonl","--output-directory",root/"unreliable-isolation"))
        q=lines(root/"unreliable-isolation/player-quality.jsonl")
        assert res["samples"]==40 and res["errors"]==1,res
        assert all(s["data"]["status"]=="plausible" for s in q[-2:]),q[-2:]
        # Packet framing errors break continuity, but later complete blocks can recover.
        bad_packet=event(W().bits(0,8),23)
        write(root/"packet-error.jsonl",records+[bad_packet,after_close])
        run(exe,"decode-players","--in",root/"packet-error.jsonl","--output-directory",root/"packet-error")
        q=lines(root/"packet-error/player-quality.jsonl")
        assert len(q)==38 and all(s["data"]["status"]=="warming_up" for s in q[-2:]),q[-2:]
        assert all(s["data"]["flags"]==["decoder_continuity_reset"] for s in q[-2:]),q[-2:]
        # Adjacent packets can share a capture millisecond without being out of order.
        coincident=event(packet([bunch(movement_payload(offset=18),seq=20)],23),23)
        coincident["data"]["timestampMs"]=records[-1]["data"]["timestampMs"]
        write(root/"same-millisecond.jsonl",records+[coincident])
        run(exe,"decode-players","--in",root/"same-millisecond.jsonl","--output-directory",root/"same-millisecond")
        same_q=lines(root/"same-millisecond/player-quality.jsonl")
        assert len(same_q)==38 and all(s["data"]["status"]=="plausible" for s in same_q[-2:]),same_q[-2:]
        assert all(s["data"]["intervalSource"]=="capture_packet_order_same_millisecond" and
                   s["data"]["timestampMs"]==coincident["data"]["timestampMs"] for s in same_q[-2:]),same_q[-2:]
        # Actual encrypted replay -> bank binding -> plaintext -> native players -> validation.
        bank=bytes((i*37+29)&255 for i in range(128));(root/"bank.bin").write_bytes(bank)
        raw=[]
        for e in records:
            d=e["data"];plain=base64.b64decode(d["payloadBase64"])
            raw.append({"type":"udp_raw","timestampMs":d["timestampMs"],"data":{
                "direction":"down","local":d["local"],"remote":d["remote"],
                "payloadBase64":base64.b64encode(crypt_udp(plain,bank)).decode()}})
        write(root/"raw.jsonl",raw)
        run(exe,"replay","--in",root/"raw.jsonl","--out",root/"replay.jsonl",
            "--device","synthetic-player-test",root/"bank.bin")
        replay=lines(root/"replay.jsonl")
        assert len([e for e in replay if e["type"]=="player_sample"])==36
        assert len([e for e in replay if e["type"]=="player_validation"])==36
        assert next(e["data"] for e in replay if e["type"]=="player_decoder_summary")["errors"]==0
        assert all(e["synthetic"] for e in replay)
        # A recognized unrelated actor is ignored without applying a player schema.
        unrelated=event(packet([bunch(actor_prefix(100,101,"UnrelatedActor").join(W().bits(0xff,8)),
                                      channel=99,opened=True,exports=True)],100),100)
        write(root/"unrelated.jsonl",[unrelated])
        unrelated_summary=json.loads(run(exe,"decode-players","--in",root/"unrelated.jsonl",
                                         "--output-directory",root/"unrelated"))
        assert unrelated_summary["samples"]==unrelated_summary["errors"]==0,unrelated_summary
        # Unreliable partials in adjacent packets work; a missing packet invalidates the chain.
        whole=movement_payload();a=W();a.b=whole.b[:91];b=W();b.b=whole.b[91:]
        for gap_between in (False,True):
            prefix=player_records(1)
            n=7 if gap_between else 6
            fragments=[event(packet([bunch(a,partial=(True,False),reliable=False)],6),6),
                       event(packet([bunch(b,partial=(False,True),reliable=False)],n+1),n+1)]
            label="partial-gap" if gap_between else "partial-ok"
            write(root/(label+".jsonl"),prefix+fragments)
            res=json.loads(run(exe,"decode-players","--in",root/(label+".jsonl"),"--output-directory",root/label))
            assert res["samples"]==(2 if gap_between else 4),res
            assert bool(res["errors"])==gap_between,res
    print(json.dumps({"nativePlayerSamples":36,"encryptedReplaySamples":36,
        "channelCloseAndReuse":True,"gapAndEpochReset":True,"errorContinuity":True,"realGameCapture":False}))


if __name__=="__main__":
    main()

"""Nonempty synthetic UE packets compared with recovered Python 3.14 decoders."""
import base64
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
os.environ["ER_ROOT"]=str(ROOT/"runtime/ErCore")
sys.path.insert(0,str(ROOT/"runtime/vendor_pyc"))
if sys.version_info[:2] != (3,14):
    print("SKIP: reference bytecode requires Python 3.14")
    raise SystemExit(77)
try:
    import numpy
except ImportError:
    print("SKIP: reference decoder requires numpy")
    raise SystemExit(77)
import repmove4
import playerstate_protocol
import er_live
import repdec4
import sna
import c2s_self
from codec.engine_packet import decode_engine_packet
from codec.bitstream import BitReader


from player_fixtures import *


def request(w,mode,**kwargs):return dict(mode=mode,hex=w.raw().hex(),bits=len(w.b),**kwargs)
def run(exe,*args):
    p=subprocess.run([str(exe),*map(str,args)],capture_output=True,text=True,encoding="utf-8",errors="replace")
    assert p.returncode==0,(p.returncode,p.stdout,p.stderr)
    return p.stdout
def lines(path):return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def main():
    exe,probe=map(Path,sys.argv[1:])
    requests=[];expected=[];rng=random.Random(42713)
    for full in (True,False):
        for i in range(80):
            w=self_payload(ts=10+i*.1,loc=(rng.uniform(5000,1500000),rng.uniform(-1500000,1500000),-10000),
                yaw=rng.uniform(-360,360),pitch=rng.uniform(-180,180),full=full,
                vel=(rng.uniform(-1000,1000),0,0),accel=(0,100,0))
            requests.append(request(w,'self'));expected.append(('self',c2s_self.parse_fgp(w.raw(),len(w.b))))
    for w in (self_payload(ts=0),self_payload(ts=float('nan')),self_payload(loc=(1,2,3)),
              self_payload(flags=255),self_payload().bits(1,1)):
        requests.append(request(w,'self'));expected.append(('self',c2s_self.parse_fgp(w.raw(),len(w.b))))
    w=self_payload()
    for cut in (1,8,30,48,100,150):
        truncated=W();truncated.b=w.b[:-cut]
        requests.append(request(truncated,'self'));expected.append(('self',c2s_self.parse_fgp(truncated.raw(),len(truncated.b))))
    for i in range(240):
        slots=sorted(rng.sample(range(80),rng.randrange(1,15)))
        w=frame(slots,rng,embedded=i%3==0)
        native=request(w,"movement");reference,_=repmove4.read_frame4(w.raw(),0,len(w.b))
        requests.append(native);expected.append(("movement",reference))
    for count in (0,1,16,17,32,512):
        w=frame((),count=count);reference,_=repmove4.read_frame4(w.raw(),0,len(w.b))
        requests.append(request(w,"movement"));expected.append(("movement",reference))
    # Compare the exact stopping offset and only the prefix emitted before an unknown 0x10 object.
    for mask in (16,17,19,31,81,4095):
        for stop_slot in (0,17,33):
            w=frame((0,17,33),masks={stop_slot:mask})
            ref,_=repmove4.read_frame4(w.raw(),0,len(w.b))
            requests.append(request(w,"movement"));expected.append(("movement",ref))
    w=frame(embedded=True,embedded_mask=81)
    ref,_=repmove4.read_frame4(w.raw(),0,len(w.b))
    requests.append(request(w,"movement"));expected.append(("movement",ref))
    for cut in (1,17,34,65,100):
        w=frame();w.b=w.b[:-cut];requests.append(request(w,"movement"));expected.append(("error",None))
    for text in ("Ascii name","玩家名字🙂","", "é\u00a0"):
        for w in (W().string(text),W().utf8(text)):
            ref=BitReader(w.raw(),len(w.b)).fstring()
            requests.append(request(w,"string"));expected.append(("string",ref))
    for quantized in (False,True):
        w=actor_prefix(4,5,"BP_DFMCharacter_C",quantized,True)
        ref,_=sna.parse_prefix(w.b,True,False,quantize_bit=quantized)
        requests.append(request(w,"prefix",exports=True,quantizedBit=quantized));expected.append(("prefix",ref))
    # Compare both PlayerState schema versions, enum widths, UTF-16, checksums and dynamic arrays.
    for profile in ("current","legacy"):
        cat,parents,meta=playerstate_protocol._schema(profile)
        commands=repdec4.cmd_list(cat)
        handle_by_name={parents[c["parent_index"]]["name"]:c["handle"] for c in commands if c["type"]!=1}
        for i in range(32):
            w=W().bit(i%2);names=("PlayerId","PlayerNamePrivate","Uin","TeamID")
            for name in sorted(names,key=lambda n:handle_by_name[n]):
                w.packed(handle_by_name[name])
                if i%2:w.bits(0x1234,32)
                if name=="PlayerNamePrivate":w.string("玩家 Player "+str(i))
                elif name=="Uin":w.bits(100000000000+i,64)
                else:w.bits(i+7,32)
            w.packed(0)
            if i%2:w.bits(0,32)
            reference=playerstate_protocol.decode_playerstate(w.raw(),0,len(w.b),profile)
            assert reference["status"]=="exact",reference
            requests.append(request(w,"properties",profile=profile));expected.append(("properties",reference))
        # Two sparse array elements, followed by a top-level scalar, with/without checksums.
        array=next(c for c in commands if c["type"]==0 and parents[c["parent_index"]]["name"]=="MemberInfoList")
        width=array["end_cmd"]-array["index"]-2
        for checksum in (False,True):
            w=W().bit(checksum)
            def h(n):
                w.packed(n)
                if checksum:w.bits(0x1234,32)
            h(array["handle"]);w.bits(2,16)
            h(1);w.bits(111,64)
            h(width+2);w.bits(222,32)
            h(0);h(handle_by_name["TeamID"]);w.bits(7,32);h(0)
            ref=playerstate_protocol.decode_playerstate(w.raw(),0,len(w.b),profile)
            assert ref["status"]=="exact",ref
            requests.append(request(w,"properties",profile=profile));expected.append(("properties",ref))
    char=W().bit(0).packed(14).packed(6).packed(18).packed(6).packed(60).packed(17).packed(0)
    cat=repdec4.load_catalogs()["BP_DFMCharacter_C"]
    ref=er_live.decode_character_identity(char.raw(),0,len(char.b),repdec4.cmd_list(cat),{p["index"]:p for p in cat["parents"]})
    assert ref=={14:6,18:6,60:17},ref
    requests.append(request(char,"properties",profile="character"));expected.append(("character",ref))
    records=[]
    welcome=W().bits(1,8).string("/Game/TestMap")
    records.append(event(packet([bunch(welcome,channel=0,seq=1)],1),1))
    ps=W().bit(0).packed(27).string("测试玩家").packed(62).bits(7,32).packed(0)
    records.append(event(packet([bunch(actor_prefix(6,7,"BP_SOLPlayerState_C").join(content(ps)),channel=4,seq=1,opened=True,exports=True)],2),2))
    records.append(event(packet([bunch(actor_prefix(4,5,"BP_DFMCharacter_C",True,True).join(content(char)),channel=3,seq=1,opened=True,exports=True)],3),3))
    records.append(event(packet([bunch(actor_prefix(2,3,"MovementReplicationActor").join(movement_payload()),seq=1023,opened=True,exports=True)],4),4))
    records.append(event(packet([bunch(movement_payload(),seq=0)],5),5))
    records.append(records[-1])  # packet duplicate
    # Out-of-order reliable fragments and 1023 -> 0 sequence wrap.
    whole=movement_payload();a=W();a.b=whole.b[:91];b=W();b.b=whole.b[91:]
    records.append(event(packet([bunch(b,seq=2,partial=(False,True))],7),7))
    records.append(event(packet([bunch(a,seq=1,partial=(True,False))],6),6))
    # An unsupported object keeps the current known coordinate, never subsequent slots.
    records.append(event(packet([bunch(movement_payload(mask=17),seq=3)],8),8))
    for i,record in enumerate(records):
        raw=base64.b64decode(record["data"]["payloadBase64"])
        trace=decode_engine_packet(i,"response",raw,request_prefix=False)
        assert not trace.error and not trace.bunch_parse.error,trace.as_dict()
        requests.append(dict(mode="packet",hex=raw.hex()));expected.append(("packet",trace))
    result=subprocess.run([str(probe)],input="\n".join(json.dumps(r) for r in requests)+"\n",
        capture_output=True,text=True,encoding="utf-8",timeout=30)
    assert result.returncode==0,result.stderr
    actual=[json.loads(line) for line in result.stdout.splitlines()]
    assert len(actual)==len(expected)
    for index,(out,(kind,ref)) in enumerate(zip(actual,expected)):
        if kind=="error":assert not out["ok"],(index,out);continue
        assert out["ok"],(index,kind,out)
        got=out["result"]
        if kind=="movement":
            assert got["items"]=={str(k):v for k,v in ref["items"].items()},(index,got,ref)
            assert got["pos"]==ref["pos"] and got["left"]==ref["left"],index
            assert got["complete"] == (ref.get("stopped") is None),(index,got,ref)
        elif kind=="properties":
            assert got["status"]==ref["status"],(index,got,ref)
            for k,v in ref["fields"].items():assert got["fields"][k]==v,(index,k,got,ref)
        elif kind=="string":assert got==ref,(index,got,ref)
        elif kind=="self":assert got==ref,(index,got,ref)
        elif kind=="prefix":
            for k in ("actor","arch","level","loc","rot","scale","vel","stream_start"):
                assert got[k]==ref[k],(index,k,got,ref)
        elif kind=="character":assert got["fields"]["MoveHandle"]==17 and got["fields"]["PlayerState"]["guid"]==6
        elif kind=="packet":
            assert got["sequence"]==ref.packet.packet_sequence
            assert len(got["bunches"])==len(ref.bunch_parse.bunches)
            for b,r in zip(got["bunches"],ref.bunch_parse.bunches):
                assert b["channel"]==r.channel_index and b["bits"]==r.payload_bit_count and b["payload"]==r.payload.hex()
    with tempfile.TemporaryDirectory(prefix="radaer-native-players-") as temporary:
        folder=Path(temporary);source=folder/"plain.jsonl"
        source.write_text("".join(json.dumps(e)+"\n" for e in records),encoding="utf-8")
        summary=json.loads(run(exe,"decode-players","--in",source,"--output-directory",folder/"decoded"))
        samples=lines(folder/"decoded/players.jsonl")
        assert len(samples)==7,(summary,samples,lines(folder/"decoded/decoder-errors.jsonl"))
        partial=samples[-1]["data"]
        assert partial["slot"]==0 and partial["decodeCompleteness"]=="known_prefix",partial
        assert not partial["movementFrameComplete"] and partial["x"]==125.4,partial
        assert all(s["data"]["handshakeOk"] for s in samples)
        named=[s for s in samples if s["data"]["slot"]==17]
        assert len(named)==3 and all(s["data"]["name"]=="测试玩家" and s["data"]["teamId"]==7 for s in named),named
        assert all(s["data"]["x"]==142.4 and s["data"]["yaw"]==90 for s in named),named
        assert summary["errors"]==0 and summary["warnings"]==0 and summary["duplicatePacketsOrBunches"]>=1,summary
        # Compare nonempty positions with the complete prior Python bridge on ordered, valid packets.
        reference_input=folder/"reference.jsonl"
        # LiveView locks an unidentified movement channel only after 16 h21 records.
        ordered=records[:3]+[event(packet([bunch(actor_prefix(2,3,"MovementReplicationActor"),seq=1,opened=True,exports=True)],4),4)]
        ordered += [event(packet([bunch(movement_payload(offset=i),seq=i+2)],i+5),i+5) for i in range(18)]
        reference_input.write_text("".join(json.dumps(e)+"\n" for e in ordered),encoding="utf-8")
        run(Path(sys.executable),ROOT/"native/tools/decode_players.py","--input",reference_input,"--output",folder/"python")
        python_samples=lines(folder/"python/players.jsonl")
        assert len(python_samples)==36,python_samples
        run(exe,"decode-players","--in",reference_input,"--output-directory",folder/"comparison")
        native_comparison=lines(folder/"comparison/players.jsonl")
        ordered_samples=lambda rows: sorted((r["data"] for r in rows),key=lambda d:(d["slot"],d["timestampMs"]))
        assert len(native_comparison)==len(python_samples)
        for a,b in zip(ordered_samples(native_comparison),ordered_samples(python_samples)):
            assert abs(a["timestampMs"]-b["timestampMs"])<=1,(a,b)
            assert all(a[k]==b[k] for k in ("slot","x","y","z","yaw")),(a,b)
        # Bank rebind and capture gaps must invalidate explicit identity associations.
        reset={"type":"capture_gap","session":"ue-test","data":{"reason":"test"}}
        source2=folder/"reset.jsonl"
        source2.write_text("".join(json.dumps(e)+"\n" for e in records[:5]+[reset,event(packet([bunch(movement_payload(),seq=4)],9),9)]))
        run(exe,"decode-players","--in",source2,"--output-directory",folder/"reset")
        last=lines(folder/"reset/players.jsonl")[-2:]
        assert all(s["data"]["name"] is None and not s["data"]["handshakeOk"] for s in last),last
    print(json.dumps({"differentialVectors":len(expected),"nonemptyNativeSamples":36,
        "nonemptyPythonSamples":36,"identityAndTeamVerified":True,"reorderedPartialAndWrapVerified":True,
        "unsupportedMovementStopsAtKnownPrefix":True,"realGameCapture":False}))

if __name__=="__main__":main()

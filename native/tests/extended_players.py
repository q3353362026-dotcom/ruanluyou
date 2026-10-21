"""Exercise new motion deltas and separate downstream self data from real UE wire fixtures."""
import json, sys, tempfile
from pathlib import Path
from player_fixtures import *
from player_integration import run, write, lines

def extended_body(mask=97, embedded=True):
    w=W().bits(18,32).bits(1<<17,32)
    ctx=header(w)
    w.bits(3,2).bits(mask,12)
    if mask&64:w.bits(123456*2,32)  # 12.3456 seconds
    if mask&1:
        for v in (10000.1,-20000.2,300.3):
            q=round(v*10);w.bits((q<<1)^(q>>31),32)
        quantized(w,(300,-400,0),1,24)
        w.bits(0,2).bit(0).bit(1).bits(64,8).bit(0)
    if mask&32:w.bits((16384<<16)|49152,32)
    if mask&16:return w
    w.bit(0).bit(0).bit(embedded)
    if embedded:item(w,897,header(w),pos=(10000.1,-20000.2,300.3))
    return w.bits(42,32)

exe=Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='radaer-extended-') as t:
    root=Path(t);records=player_records()[:4]
    for n in range(5,9):
        records.append(event(packet([bunch(content(W().bit(0).packed(21).join(extended_body()).packed(0)),seq=n-3)],n),n))
    delta=event(packet([bunch(content(W().bit(0).packed(21).join(extended_body(32,False)).packed(0)),seq=6)],9),9)
    records.append(delta)
    partial=event(packet([bunch(content(W().bit(0).packed(21).join(extended_body(113,False))),seq=7)],10),10)
    records.append(partial)
    records.append(self_event(11,vel=(1200,100,0),accel=(300,-400,0)))
    records.append(self_event(12,vel=(1200,100,0),full=False))
    write(root/'input.jsonl',records)
    summary=json.loads(run(exe,'decode-players','--in',root/'input.jsonl','--output-directory',root/'out'))
    assert summary['samples']==5 and summary['selfSamples']==2 and summary['serverSelfSamples']==4,summary
    assert summary['motionUpdates']==1 and summary['errors']==summary['warnings']==summary['exceptions']==0,summary
    players=[e['data'] for e in lines(root/'out/players.jsonl')]
    m=players[0]['motion']
    assert m['velocity']==[300,-400,0] and m['viewYaw']==90 and m['viewPitch']==-90,m
    assert m['timeSeconds']==12.3456,m
    assert players[-1]['motion']=={} and players[-1]['decodeCompleteness']=='known_prefix'
    updates=lines(root/'out/motion-updates.jsonl')
    assert updates[0]['data']['track']==players[0]['track']
    assert updates[0]['data']['timestampMs']>players[-2]['timestampMs']
    server=lines(root/'out/self-server-quality.jsonl')
    assert server[-1]['data']['status']=='plausible' and server[-1]['data']['actorGuid']==4,server[-1]
    assert all(e['type']=='self_server_validation' for e in server)
    selfs=[e['data'] for e in lines(root/'out/self.jsonl')]
    assert selfs[0]['velocity']==[300,-400,0] and selfs[0]['acceleration']==[1200,100,0],selfs[0]
    assert selfs[1]['velocity'] is None and selfs[1]['motion']['velocity'] is None
    # No persisted source derived events may be fed back into the new replay.
    write(root/'derived.jsonl',records+updates+server)
    again=json.loads(run(exe,'decode-players','--in',root/'derived.jsonl','--output-directory',root/'again'))
    assert again==summary,(again,summary)
print('Extended wire values, view-only deltas, partial boundary, full/lite upstream and server-self validation passed')

"""Bounded health wire parsing, original bytecode differential and full event integration."""
import json, os, random, struct, subprocess, sys, tempfile
from pathlib import Path
from player_fixtures import *
from player_integration import run, write, lines

def health_body(values, dense=True, count=92, index=91):
    item=W()
    for h,v in sorted(values.items()):
        item.packed(h)
        if h==7:item.packed(v)
        else:item.bits(struct.unpack('<I',struct.pack('<f',v))[0],32)
    item.packed(0)
    w=W().bit(0).packed(3).bit(dense).packed(count)
    if dense:
        for i in range(count):w.bit(i==index)
    else:w.bits(index+1,16).bits(0,16)
    return w.packed(len(item.b)).join(item).packed(0)

def sub(body):return W().bit(1).bit(0).packed(13).bit(1).packed(len(body.b)).join(body)
def exports(entries):
    w=W().bit(0).bits(len(entries),32)
    for guid,outer,path in entries:
        w.packed(guid).bits(1,8).packed(outer)
        if outer:w.bits(0,8)
        w.string(path)
    return w

def main():
    exe,probe=map(Path,sys.argv[1:])
    ref=None
    if sys.version_info[:2]==(3,14):
        root=Path(__file__).resolve().parents[2];os.environ['ER_ROOT']=str(root/'runtime/ErCore')
        sys.path.insert(0,str(root/'runtime/vendor_pyc'))
        try:import property_health as ref
        except ImportError:pass
    rng=random.Random(651);cases=[]
    for n in range(120):
        vals={h:rng.randint(0,1000)/4 for h in rng.sample(range(1,7),rng.randint(1,6))};vals[7]=8+n*2
        cases.append((health_body(vals,bool(n%2)),True))
    good=health_body({1:75,6:3,7:15})
    for cut in range(1,len(good.b)):
        w=W();w.b=good.b[:cut];cases.append((w,False))
    for v in [float('nan'),float('inf'),-float('inf')]:cases.append((health_body({1:v}),False))
    cases.append((W().bit(1).packed(0),False))
    cases.append((good.join(W().bit(1)),False))
    request=[dict(mode='health',hex=w.raw().hex(),bits=len(w.b)) for w,_ in cases]
    p=subprocess.run([str(probe)],input='\n'.join(map(json.dumps,request))+'\n',capture_output=True,text=True,timeout=30)
    assert p.returncode==0,p.stderr
    out=list(map(json.loads,p.stdout.splitlines()));assert len(out)==len(cases)
    for (w,valid),result in zip(cases,out):
        assert result['ok']==valid,result
        if ref:
            try:expected=ref.decode_health_block(w.raw(),dict(payload_start=0,payload_end=len(w.b)))
            except Exception:assert not valid
            else:assert json.loads(json.dumps(expected))==result['result']
    with tempfile.TemporaryDirectory(prefix='radaer-health-') as t:
        root=Path(t);records=player_records(3)
        start=exports([(11,0,'BP_PropertyReplicationCharacterHealth_C'),(13,10,'PropertyReplication'),(15,4,'AttributeCenter')]).packed(10).packed(11).packed(0).bits(0,4)
        records.append(event(packet([bunch(start.join(sub(health_body({1:75,6:1,7:15}))),channel=17,opened=True,exports=True)],8),8))
        for n,(hp,life) in enumerate([(0,3),(0,0),(0,2),(20,1)],9):
            records.append(event(packet([bunch(sub(health_body({1:hp,6:life})),channel=17,seq=n-7)],n),n))
        # Two life changes in one network packet must preserve their bunch order.
        records.append(event(packet([bunch(sub(health_body({6:2})),channel=17,seq=6),bunch(sub(health_body({6:1})),channel=17,seq=7)],13),13))
        records.append(event(packet([bunch(W(),channel=3,seq=2,closed=True,close_reason=1)],14),14))
        records.append(self_event(14))
        records.append(event(packet([bunch(movement_payload(),seq=5)],15),15))
        records.append(event(packet([bunch(W().packed(4),channel=3,seq=3,opened=True,closed=True)],16),16))
        records.append(event(packet([bunch(movement_payload(),seq=6)],17),17))
        records.append(self_event(18))
        write(root/'in.jsonl',records)
        summary=json.loads(run(exe,'decode-players','--in',root/'in.jsonl','--output-directory',root/'out'))
        assert summary['errors']==summary['warnings']==summary['exceptions']==0,summary
        events=lines(root/'out/player-state.jsonl');states=[e['data'] for e in events]
        health=[s for s in states if s['vitals'].get('health') and 'lifecycle' not in s or s['vitals'].get('life',{}).get('source')=='property_replication']
        assert {s['actorGuid'] for s in health}=={4},health
        assert any(s['vitals']['life']['state']=='downed' and s['vitals']['health']['Health']['value']==0 for s in health)
        at13=[s['vitals']['life']['state'] for s in states if s['timestampMs']==1700000000130]
        assert at13==['dead','alive'],at13
        assert any(s.get('lifecycle',{}).get('reason')=='dormancy' and s['lifecycle']['present'] for s in states)
        poses=[e['data'] for e in lines(root/'out/players.jsonl') if e['data'].get('actorGuid')==4]
        assert poses[-1]['timestampMs']==1700000000150 and poses[-1]['vitals']['life']['state']=='alive',poses[-1]
        assert poses[-1]['vitals']['health']['Health']['value']==20
        own=lines(root/'out/self.jsonl');assert len(own)==1 and own[0]['data']['actorGuid']==4,'dormancy preserves self identity; destroyed self cannot reappear unbound'
        assert not any(e['data']['slot']==17 and e['data']['timestampMs']==1700000000170 for e in lines(root/'out/players.jsonl')),'closed character cannot reappear as anonymous slot'
        write(root/'derived.jsonl',records+events)
        again=json.loads(run(exe,'decode-players','--in',root/'derived.jsonl','--output-directory',root/'again'))
        assert again==summary,'derived state events must not be replayed twice'
        relevancy=json.loads(json.dumps(records[:8]))
        relevancy.append(event(packet([bunch(W(),channel=3,seq=2,closed=True,close_reason=3)],30),30))
        relevancy.append(event(packet([bunch(movement_payload(),seq=5)],31),31))
        write(root/'relevancy.jsonl',relevancy)
        run(exe,'decode-players','--in',root/'relevancy.jsonl','--output-directory',root/'relevancy')
        re=lines(root/'relevancy/player-state.jsonl')[-1]['data']
        assert re['lifecycle']['present'] and re['lifecycle']['reason']=='out_of_relevancy',re
        poses=[e['data'] for e in lines(root/'relevancy/players.jsonl') if e['data'].get('actorGuid')==4]
        assert poses[-1]['timestampMs']==1700000000310,'relevancy closure retains explicit movement identity'
    print(f'Health wire: {len(cases)} cases; original differential={bool(ref)}; independent HP/life, same-packet revival and dormancy/removal passed')

if __name__=='__main__':main()
